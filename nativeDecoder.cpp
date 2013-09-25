#include <jni.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <linux/tcp.h>
#include <fcntl.h>


#define LOG_TAG "NativeDeocder"
#include <utils/Log.h>

#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <media/ICrypto.h>
#include <media/IMediaPlayerService.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AString.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/NuMediaExtractor.h>
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
#include <ui/DisplayInfo.h>

extern "C" void rtp_mpeg_recvfrom(unsigned char *msg, int len);
extern "C" int ts_parse(unsigned char *buf, int length);

static int av_sync = 0;
static int quit = 0;
static int recv_port = 5555;
static int video_width = 1920;
static int video_height = 1080;

#define BUFFER_SIZE     0x100000
typedef struct {
	unsigned long timestamp;
	unsigned long size;
	unsigned char data[BUFFER_SIZE];
} video_buf;

#define VBUF_NUM 32
static video_buf vbuf[VBUF_NUM];
static int vbuf_wptr, vbuf_rptr;
static unsigned int video_decode_pts;
static unsigned int video_start_ts, video_start_time;
static pthread_mutex_t vbuf_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vbuf_cond = PTHREAD_COND_INITIALIZER;
extern "C" void queue_video_buf(unsigned char *buf, int len, unsigned int timestamp)
{
	int info  = 0;
	unsigned char type = (buf[4]) & ((1<<5)-1);
	//if (type == 0xc) return;

	if (len > 0x100000) {
		ALOGV("queue_video_buf: data is bigger than buf");
		return;
	}
	//ALOGV("queue video buf len=%d ts=%d\n", len, timestamp);

	memcpy(vbuf[vbuf_wptr].data, buf, len);
	vbuf[vbuf_wptr].size = len;
	vbuf[vbuf_wptr].timestamp = timestamp;

	pthread_mutex_lock(&vbuf_mutex);
	if (vbuf_rptr == vbuf_wptr)
		info = 1;
	vbuf_wptr = (vbuf_wptr + 1) % VBUF_NUM;

	if (vbuf_wptr == vbuf_rptr) {
		printf("video overflow\n");
	}

	if (info == 1) {
		pthread_cond_signal(&vbuf_cond);
	}
	pthread_mutex_unlock(&vbuf_mutex);

	return;
}

#define ABUFFER_SIZE    2048
typedef struct {
	unsigned long timestamp;
	unsigned long size;
	unsigned char data[ABUFFER_SIZE];
} audio_buf;

#define ABUF_NUM 128
static audio_buf abuf[ABUF_NUM];
static int abuf_wptr, abuf_rptr;
static unsigned int audio_decode_pts;
static unsigned int audio_start_ts, audio_start_time;
static pthread_mutex_t abuf_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t abuf_cond = PTHREAD_COND_INITIALIZER;
extern "C" void queue_audio_buf(unsigned char *buf, int len, unsigned int timestamp)
{
	int info = 0;

	if (len > ABUFFER_SIZE) {
		ALOGV("queue_audio_buf: data is bigger than buf");
		return;
	}
	//ALOGV("queue audio buf len=%d ts=%d", len, timestamp);

	memcpy(abuf[abuf_wptr].data, buf, len);
	abuf[abuf_wptr].size = len;
	abuf[abuf_wptr].timestamp = timestamp;

	pthread_mutex_lock(&abuf_mutex);
	if (abuf_rptr == abuf_wptr)
		info = 1;
	abuf_wptr = (abuf_wptr + 1) % ABUF_NUM;
	if (info == 1) {
		pthread_cond_signal(&abuf_cond);
	}
	if (abuf_rptr == abuf_wptr)
		printf("audio overflow\n");
	pthread_mutex_unlock(&abuf_mutex);
	return;
}

static int aacd_adts_sync(unsigned char *buffer, int len)
{
    int pos = 0;
    len -= 3;

    //LOGV("probe() start len=%d", len);
    while (pos < len) {
        if (*buffer != 0xff) {
            buffer++;
            pos++;
        } else if ((*(++buffer) & 0xf6) == 0xf0) {
            //LOGV("probe() found ADTS start at offset %d", pos );
            return pos;
        }
        else pos++;
    }
    //LOGV("probe() could not find ADTS start" );
    return 0;
}

static unsigned char recvbuf[4096];
static void *udp_recv(void* arg)
{
	int sd;
	struct sockaddr_in addr;
	int nread;
	int yes = 1;

	struct timeval tv;

	sd = socket(PF_INET, SOCK_DGRAM, 0);
	setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(recv_port);
	addr.sin_addr.s_addr = INADDR_ANY;

	nread = bind(sd, (struct sockaddr *)&addr, sizeof(addr));
	if (nread < 0) {
		ALOGV("oops bind error");
		return NULL;
	}

	tv.tv_sec = 1;
	tv.tv_usec = 0;
	setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(struct timeval));

	while (!quit) {
		nread = recvfrom(sd, recvbuf, 4096, 0, (struct sockaddr *)NULL, 0);
		if (nread > 0)
			rtp_mpeg_recvfrom(recvbuf, nread);
	}

	close(sd);
	return NULL;
}

static void *read_file(void *arg)
{
	int fd;
	unsigned char buf[256];
	int rn;
	fd = open("/sdcard/NativeMedia.ts", O_RDONLY);
	
	while (!quit && (rn = read(fd, buf, 188) > 0)) {
		 ts_parse(buf, rn);
		 usleep(1);
	}
	
	return NULL;
}

static void video_decode(void)
{
	using namespace android;
	
	/*
	 * Create Surface
	 */
	sp<SurfaceComposerClient> composerClient = new SurfaceComposerClient;
	sp<SurfaceControl> control;
	sp<Surface> surface;
    
	control = composerClient->createSurface(String8(LOG_TAG), 1280, 800, PIXEL_FORMAT_RGB_565, 0);
	SurfaceComposerClient::openGlobalTransaction();
	control->setLayer(21015);
	control->show();
	SurfaceComposerClient::closeGlobalTransaction();
	surface = control->getSurface();
	
	/*
	 * MediaCodec
	 */ 
	status_t err;
	sp<ALooper> looper = new ALooper;
	looper->start();
	
	sp<MediaCodec> mCodec;
	Vector<sp<ABuffer> > mInBuffers;
	Vector<sp<ABuffer> > mOutBuffers;
	
	mCodec = MediaCodec::CreateByType(looper, "video/avc", false);
	
	sp<AMessage> format;
	format = new AMessage;
	format->setString("mime", "video/avc");
	format->setInt32("width", video_width);
	format->setInt32("height", video_height);
	
	err = mCodec->configure(format, surface, NULL, 0);
	if (err != OK) {
		ALOGV("Configure Video Codec Failed!");
		quit = 1;
		return;
	}
	
	mCodec->start();
	mCodec->getInputBuffers(&mInBuffers);
	mCodec->getOutputBuffers(&mOutBuffers);
	ALOGV("got %d input and %d output buffers", mInBuffers.size(), mOutBuffers.size());
	
	int64_t kTimeout = -1ll;
	size_t index;
	while (!quit) {
		err = mCodec->dequeueInputBuffer(&index, kTimeout);
		if (err != OK) {
			ALOGV("Video Codec Dequeue Input Buffer Error!");
			continue;
		}
		
		const sp<ABuffer> &buffer = mInBuffers.itemAt(index);
		// copy h264 to buffer->data()
		pthread_mutex_lock(&vbuf_mutex);
		while(!quit) {
			if (vbuf_rptr != vbuf_wptr)
				break;
			pthread_cond_wait(&vbuf_cond, &vbuf_mutex);
		}
		memcpy(buffer->data(), vbuf[vbuf_rptr].data, vbuf[vbuf_rptr].size);
		buffer->setRange(0, vbuf[vbuf_rptr].size);
		vbuf_rptr = (vbuf_rptr + 1) % VBUF_NUM;
		pthread_mutex_unlock(&vbuf_mutex);
		// END copy h264 to buffer->data()

		err = mCodec->queueInputBuffer(index, 0, buffer->size(), ALooper::GetNowUs(), 0);
		if (err != OK) {
			ALOGV("Video Codec Queue Input Buffer Error!");
			continue;
		}
		
		size_t offset;
		size_t size;
		int64_t presentationTimeUs;
		uint32_t flags;
		err = mCodec->dequeueOutputBuffer(&index, &offset, &size, &presentationTimeUs, &flags, 10000ll);
		if (err == OK) {
			err = mCodec->renderOutputBufferAndRelease(index);
		} else if (err == INFO_OUTPUT_BUFFERS_CHANGED) {
			ALOGV("INFO_OUTPUT_BUFFERS_CHANGED");
			mCodec->getOutputBuffers(&mOutBuffers);
			ALOGV("got %d output buffers", mOutBuffers.size());
		} else if (err == INFO_FORMAT_CHANGED) {
			sp<AMessage> format;
			mCodec->getOutputFormat(&format);
			ALOGV("INFO_FORMAT_CHANGED: %s", format->debugString().c_str());
		} 
	}

	mCodec->release();
	looper->stop();
	return;
}

static void term_proc(int arg)
{
	printf("got signal\n");
	quit = 1;
}

int main(int argc, char *argv[])
{
	pthread_t recv_tid;
	pthread_t vdec_tid;
	quit = 0;
	vbuf_wptr = 0;
	vbuf_rptr = 0;
	abuf_wptr = 0;
	abuf_rptr = 0;
	
	video_decode_pts = 0;
	audio_decode_pts = 0;
	
	video_start_ts = 0;
	video_start_time = 0;
	
	audio_start_ts = 0;
	audio_start_time = 0;
	
	signal(SIGINT, term_proc);
    signal(SIGKILL, term_proc);
	
	android::ProcessState::self()->startThreadPool();
	
	pthread_create(&recv_tid, NULL, udp_recv, NULL);
	video_decode();
}
