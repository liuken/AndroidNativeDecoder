#include <netinet/in.h>
#include <android/log.h>
#define TAG "WFD_MEDIASTREAM"
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__)

extern void queue_video_buf(unsigned char *buf, int len, unsigned int timestamp);

#define rtp_header_len 12

typedef struct {
  unsigned cc:4;        /* CSRC count             */
  unsigned x:1;         /* header extension flag  */
  unsigned p:1;         /* padding flag           */
  unsigned version:2;   /* protocol version       */
  unsigned pt:7;        /* payload type           */
  unsigned m:1;         /* marker bit             */
  unsigned short seq;         /* sequence number        */
  unsigned int ts;          /* timestamp              */
  unsigned int ssrc;        /* synchronization source */
} rtp_hdr_t;

#define FU_A 28

static unsigned char rtp_h264_buf[0x100000];
static unsigned int rtp_h264_buf_len = 0;

void rtp_h264_recvfrom(unsigned char *msg, int len) 
{ 
	unsigned char nri, nal_type, mark;
	unsigned char fu_indicator, fu_header;
	unsigned char h264_start_code[4] = {0, 0, 0, 1};
	rtp_hdr_t  *rtp_header;
	unsigned char *payload;
	unsigned char *buf_write;
	
	static unsigned int seq;		
		
	rtp_header = (rtp_hdr_t *)msg;

	/* verify rtp header */
	if (rtp_header->version != 2)
		return;
		
	payload = msg + rtp_header_len;
  
	mark = rtp_header->m;
	if (seq+1 != ntohs(rtp_header->seq))
		LOGV("OOPS %d %d", seq, ntohs(rtp_header->seq));
	seq = ntohs(rtp_header->seq);
	
	buf_write = rtp_h264_buf + rtp_h264_buf_len;
	nal_type = *(payload) & ((1<<5)-1);
	if (nal_type == FU_A) { //FU_A
		unsigned char start, end, nri, type, nal;
		fu_indicator = *(payload);
		fu_header = *(payload+1);
		start = fu_header>>7;
		end = (fu_header>>6) & 0x1;
		nri = (fu_indicator >> 5) & 0x3;
		type = (fu_header) & ((1<<5)-1);
		nal = ((nri&0x3)<<5) | (type & ((1<<5)-1));	
		if (start) {
			memcpy(buf_write, h264_start_code, 4);
			memcpy(buf_write+4, &nal, 1);
			memcpy(buf_write+5, payload+2, len - rtp_header_len - 2);
			rtp_h264_buf_len += (len - rtp_header_len - 2 + 4 + 1);
		} else {
			memcpy(buf_write, payload+2, len - rtp_header_len - 2);
			rtp_h264_buf_len += (len - rtp_header_len - 2);
		}
		if (end) mark = 1;
	} else { //single NALU
		memcpy(buf_write, h264_start_code, 4);
		memcpy(buf_write+4, payload, len - rtp_header_len);
		rtp_h264_buf_len += (len - rtp_header_len);
		//mark = 1;
	}
	
	if (mark) {
		queue_video_buf(rtp_h264_buf, rtp_h264_buf_len, ntohl(rtp_header->ts));
		rtp_h264_buf_len = 0;
	}
	
	return;
}

int ts_parse(unsigned char *buf, int length)
{
	int remaining = length;
	int count = 0;
	int ret = 0;
	int len = 0;
	
	while (remaining > 0) {
		int r;
		r = parse_ts_packet(buf+count*188, 188);
		count++;
		remaining -= 188;
	}

	return len;
}

void rtp_mpeg_recvfrom(unsigned char *msg, int len) 
{
	int octets_recvd;
	int payload_len;
	static unsigned int seq;
	int size;
	rtp_hdr_t  *rtp_header;
	
	rtp_header = (rtp_hdr_t *)msg;

	/* verify rtp header */
	if (rtp_header->version != 2) {
		LOGV("not rtp packet");
		return;
	}
	 
	if (seq+1 != ntohs(rtp_header->seq))
		LOGV("OOPS %d %d\n", seq, ntohs(rtp_header->seq));
	seq = ntohs(rtp_header->seq);

	size = ts_parse(msg+rtp_header_len, len - rtp_header_len);
	
	return;
}
