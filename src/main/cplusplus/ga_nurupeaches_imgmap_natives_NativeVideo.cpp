#include "ga_nurupeaches_imgmap_natives_NativeVideo.h"

#include <stdint.h>
#include <unordered_map>
#include <string>
#include <stdio.h>
#include <jvmti.h>
#include <iostream>
#include <typeinfo>

extern "C" {
	#include "libavcodec/avcodec.h"
	#include "libavformat/avformat.h"
	#include "libswscale/swscale.h"
}

using std::string;

// Basic data structure for storing minor information
struct NativeVideoContext {
	/* Very general information. */
	bool isStreaming = false;
	int width; // Target width
	int height; // Target height
	string source; // The source file we're reading from (can be local or network)

	/* Relates to/is used by libavcodec. */
	// "Raw" objects; contextless.
	AVCodec* codec;
	AVPacket packet;

	// Frames we use to go to and from the raw H264 (or whatever we're using) to basic RGB.
	AVFrame* rawFrame;
	AVFrame* rgbFrame;
	uint8_t* rgbFrameBuffer; // Buffer for avcodec to use.
	int bufferSize; // Buffer size (without sizeof(uint8_t))
	char* dbbArray; // Direct ByteBuffer array; initialized here instead of every single time we call read(). Which was totally
				  // a great design idea. Way to go me.

	// The ID of the video stream we want to get frames from. It's normally the first video stream found.
	int videoStreamId;
	// int that holds the state of a frame being finished or not.
	int frameFinished;

	// Contexts
	AVCodecContext* codecContext;
	AVFormatContext* formatContext;

	// Software scaling context.
	struct SwsContext* imgConvertContext;
};

// bool to represent whether or not we initialized avcodec and co. already.
bool initialized;

// "Global" map for relating NativeVideos (jobject) to NativeVideoContexts
//std::unordered_map<unsigned long long, NativeVideoContext*> LOOKUP_MAP;

// JVMTI "global" pointer. "JVM TI environments work across threads and are created dynamically."
jvmtiEnv* jvmti = NULL;
JavaVM* jvm = NULL;

// method ID for handleData(byte[])
jmethodID id;

inline void doCallback(JNIEnv* env, jobject callback){
	env->CallVoidMethod(callback, id);
}

/*
 * Converts a jstring to a std::string.
 */
inline string convString(JNIEnv* env, jstring jstr){
	const char* javaBytes = env->GetStringUTFChars(jstr, 0);
	string cStr = string(javaBytes);
	env->ReleaseStringUTFChars(jstr, javaBytes);
	return cStr;
}

inline void jvmtiErrorCheck(jvmtiError err, string name){
	if((int)err != 0){
		std::cout << "jvmtiError@" << name << ": " << ((int)err) << std::endl;
	}
}

inline void checkJVMTI(){
	if(jvmti == NULL){
		// get jvmti environment
		jvm->GetEnv((void**)&jvmti, (jint)JVMTI_VERSION_1_0);

		// request capabilities
		jvmtiCapabilities capabilities;
		(void)memset(&capabilities, 0, sizeof(jvmtiCapabilities));
		capabilities.can_tag_objects = 1;

		// apply capabilities and check for errors
		jvmtiError err = jvmti->AddCapabilities(&capabilities);
		jvmtiErrorCheck(err, "checkJVMTI");
	}
}

jlong getTag(jobject obj){
//	std::cout << "entry@getTag: checking jvmti" << std::endl;
	checkJVMTI();
	jlong tag = 0;
	jvmtiError err = jvmti->GetTag(obj, &tag);
	jvmtiErrorCheck(err, "getTag");
	if(tag == 0){
//		std::cout << "tag@getTag: null" << std::endl;
		return 0;
	} else {
//		std::cout << "tag@getTag: " << ((long long)tag) << std::endl;
		return tag;
	}
}

void setTag(jobject obj, long long tag){
//	std::cout << "entry@setTag: checking jvmti" << std::endl;
	checkJVMTI();
//	std::cout << "tag@setTag: " << ((long long)tag) << std::endl;
	jvmtiError err = jvmti->SetTag(obj, (jlong)tag);
	jvmtiErrorCheck(err, "setTag");
}

/*
  jvmtiError GetTag(jobject object, jlong* tag_ptr);
  jvmtiError SetTag(jobject object, jlong tag);
*/

NativeVideoContext* getContext(JNIEnv* env, jobject jthis, bool throwException){
	jlong tag_ptr = getTag(jthis);
	if(tag_ptr == 0){
		if(throwException){
			env->ThrowNew(env->FindClass("java/io/IOException"), "Failed to find a NativeVideoContext associated with "
				"this object. Perhaps something slipped and we didn't call init(int, int) first? I don't know, but this "
				"is a long error message. Why? Because I can!");
		}

		return NULL;
	} else {
		return (NativeVideoContext*)(long long)tag_ptr;
	}
}

// For the love of god, never forget this part. C++ mangler is OP!
#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT void JNICALL Java_ga_nurupeaches_imgmap_natives_NativeVideo_initialize(JNIEnv* env, jclass callingClass, jclass handlerClass){
	av_register_all();
	avcodec_register_all();
	avformat_network_init();
	env->GetJavaVM((JavaVM**)&jvm);

   	id = env->GetMethodID(handlerClass, "handleData", "()V");
   	if(!id){
		env->ThrowNew(env->FindClass("java.lang.invoke.WrongMethodTypeException"), "Failed to locate handleMethod"
			"for the given class");
		return;
   	} else {
		std::cout << "_initialize: found id" << std::endl;
   	}
}

/*
 * Natively initializes a NativeVideo.
 */
JNIEXPORT jobject JNICALL Java_ga_nurupeaches_imgmap_natives_NativeVideo__1init(JNIEnv* env, jobject jthis, jint width, jint height){
	NativeVideoContext* context = getContext(env, jthis, false);
	if(context == NULL){
		context = new NativeVideoContext;
		setTag(jthis, (long long)context);
	}

    int memorySpace = width * height * 3;
	context->dbbArray = new char[memorySpace];
    jobject directBuffer = env->NewDirectByteBuffer((void*)context->dbbArray, memorySpace);
    context->bufferSize = memorySpace;
	context->width = width;
	context->height = height;
	return directBuffer;
}

/*
 * Opens all the necessary components for "source".
 * Can return:
 * 		1 - I/O error while trying to read "source";
 			also returned along with an IOException if no NativeVideoContext was found.
 *		2 - No stream information found.
 *		3 - No video stream found.
 *		4 - No available codec to decode "source".
 *		5 - Failed to open the codec for any reason.
 *		6 - Not enough memory or general failure to allocate AVFrames.
 *		0 - Successfully opened. Not an error.
 * Also can throw:
 * 		IOException - If the NativeVideo never called init(int, int) for whatever reason.
 */
JNIEXPORT jint JNICALL Java_ga_nurupeaches_imgmap_natives_NativeVideo__1open(JNIEnv* env, jobject jthis, jstring source){
	std::cout << "entry _open: grabbing context" << std::endl;
	NativeVideoContext* context = getContext(env, jthis, true);
	std::cout << "_open: nullcheck context" << std::endl;
	if(context == NULL){
		return 1;
	}

	std::cout << "_open: setting source and formatContext" << std::endl;
	context->source = convString(env, source); // Set the source of the video here first; we'll need it.
	context->formatContext = avformat_alloc_context(); // Allocate a new format context.

	std::cout << "_open: opening input" << std::endl;
	// open the source for format inspection
	if(avformat_open_input((AVFormatContext**)&(context->formatContext), context->source.c_str(), NULL, NULL) != 0)
		return 1; // I/O open error

	std::cout << "_open: finding stream info" << std::endl;
	// find stream information
	if(avformat_find_stream_info(context->formatContext, NULL) < 0)
		return 2; // No stream info

	std::cout << "_open: finding video stream id" << std::endl;
	context->videoStreamId = -1; // initialize at -1 to see later if we found a stream
	for(unsigned int i=0; i < context->formatContext->nb_streams; i++){
		if(context->formatContext->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO){
			context->videoStreamId = i;
			break;
		}
	}

	if(context->videoStreamId == -1){
		return 3; // no video stream found
	}

	context->codecContext = context->formatContext->streams[context->videoStreamId]->codec;

	std::cout << "_open: fixing any 0 width/height" << std::endl;
	// If width is 0, assume the codec's width
	if(context->width == 0){
		context->width = context->codecContext->width;
	}

	// If height is 0, assume the codec's width
	if(context->height == 0){
		context->height = context->codecContext->height;
	}

	std::cout << "_open: finding decoder for codec" << std::endl;
	std::cout << "_open dbg: codecContext@" << (&(context->codecContext)) << std::endl;
	std::cout << "_open dbg: codec_id=" << (context->codecContext->codec_id) << std::endl;
	// find the decoder for the codec
	context->codec = avcodec_find_decoder(context->codecContext->codec_id);
	if(context->codec == NULL)
		return 4; // No codec found

	std::cout << "_open: opening decoder" << std::endl;
	// and then open it for usage
	if(avcodec_open2(context->codecContext, context->codec, NULL) < 0)
		return 5; // Failed to open codec

	std::cout << "_open: alloc frames" << std::endl;
	// allocate our frames
	context->rawFrame = av_frame_alloc();
	context->rgbFrame = av_frame_alloc();

	std::cout << "_open: nullcheck frames" << std::endl;
	// if either or null, I'm going to assume we ran out of memory.
	if(context->rawFrame == NULL || context->rgbFrame == NULL)
		return 6; // Failed to allocate frames

	std::cout << "_open: init swscale context" << std::endl;
	context->imgConvertContext = sws_getContext(
													// source dimensions
													context->codecContext->width, context->codecContext->height,
													// source pixel format
													context->codecContext->pix_fmt,

													// target dimensions
													context->width, context->height,
													// target pixel format
													PIX_FMT_RGB24,

													// rescaling functions and co.
													SWS_BICUBIC, NULL, NULL, NULL
												);


	std::cout << "_open: init buffers" << std::endl;
	context->rgbFrameBuffer = (uint8_t*)av_malloc(avpicture_get_size(PIX_FMT_RGB24, context->width, context->height) * sizeof(uint8_t));
	avpicture_fill((AVPicture*)context->rgbFrame, context->rgbFrameBuffer, PIX_FMT_RGB24, context->width, context->height);
	context->isStreaming = true;
	context->rgbFrame->data[0] = (unsigned char*)context->dbbArray;
	// Successful opening.
	return 0;
}

/*
 * Reads a frame; calls the callback's callback method when finished.
 */
JNIEXPORT void JNICALL Java_ga_nurupeaches_imgmap_natives_NativeVideo_read(JNIEnv* env, jobject jthis, jobject callback){
	NativeVideoContext* context = getContext(env, jthis, true);
	if(context == NULL || !context->isStreaming){
		return;
	}

//	std::cout << "read: reading frame" << std::endl;
	int read;
	while((read = av_read_frame(context->formatContext, &(context->packet))) >= 0){
//		std::cout << "read: recv packet" << std::endl;
		if(context->packet.stream_index == context->videoStreamId){
//			std::cout << "read: recv video packet" << std::endl;
			avcodec_decode_video2(context->codecContext, context->rawFrame, &(context->frameFinished), &(context->packet));
//			std::cout << "read: decoded video" << std::endl;

			if(context->frameFinished){
//				std::cout << "read: finished frame; scaling" << std::endl;
				sws_scale(context->imgConvertContext, (const uint8_t* const*)context->rawFrame->data,
							context->rawFrame->linesize, 0, context->codecContext->height,
            				context->rgbFrame->data, context->rgbFrame->linesize);
//				std::cout << "read: scaled image; freeing packet and breaking loop" << std::endl;

				break;
			}
		}
	}


	if(read < 0){
		std::cout << "read: read=" << read << std::endl;
		context->isStreaming = false;
		return;
	}

//	std::cout << "read: init final returning" << std::endl;
//	std::cout << "read: beforeMemcpy dbbArray@" << &(context->dbbArray) << ";typeid=" << typeid(context->dbbArray).name() << ";bufferSize=" << context->bufferSize << std::endl;
    doCallback(env, callback);
//	std::cout << "read: afterMemcpy dbbArray@" << &(context->dbbArray) << ";typeid=" << typeid(context->dbbArray).name() << ";bufferSize=" << context->bufferSize << std::endl;
}

JNIEXPORT jboolean JNICALL Java_ga_nurupeaches_imgmap_natives_NativeVideo_isStreaming(JNIEnv* env, jobject jthis){
	NativeVideoContext* context = getContext(env, jthis, false);
	if(context == NULL){
    	return false;
    }

	return context->isStreaming;
}

JNIEXPORT void JNICALL Java_ga_nurupeaches_imgmap_natives_NativeVideo_close(JNIEnv* env, jobject jthis){
	NativeVideoContext* context = getContext(env, jthis, true);
	if(context == NULL){
		return;
	}

	av_free(context->rgbFrameBuffer);
	av_free(context->rgbFrame);
	av_free(context->rawFrame);

	avcodec_close(context->codecContext);

	avformat_close_input(&(context->formatContext));

	av_free_packet(&(context->packet));
	sws_freeContext(context->imgConvertContext);
}

#ifdef __cplusplus
}
#endif
