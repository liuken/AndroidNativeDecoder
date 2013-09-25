#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "wfd.h"

#define SYNC_BYTE	0x47

AUDIO_TYPE audio_type = AAC;

static unsigned short pmt_pid = 0;
static unsigned short video_pid = 0;
static unsigned short audio_pid = 0;

#define VIDEO_BUF_CAP 0x100000
#define AUDIO_BUF_CAP 8192

unsigned char vbuf[VIDEO_BUF_CAP];
unsigned int vbuf_len = 0;

static unsigned char video_buf[VIDEO_BUF_CAP];
static unsigned char audio_buf[AUDIO_BUF_CAP];
static unsigned int video_buf_len = 0;
static unsigned int audio_buf_len = 0;

static int aac_player_socket = 0;
static struct sockaddr_in server_sockaddr;

int parse_pes(unsigned char *buf, int len, int pid, unsigned int *time)
{
	unsigned int start_code_prefix;
	unsigned char stream_id;
	unsigned short packet_length;
	
	unsigned int consumed_bytes = 0;
	
	if (len == 0 ) return 0;
	
	start_code_prefix = (buf[0] << 16) | (buf[1] << 8) | buf[2];
	stream_id = buf[3];
	packet_length = (buf[4] << 8) | buf[5];
	
	consumed_bytes += 6;
	
	if (start_code_prefix != 0x00000001) {
		return -1;
	}
	
	if (stream_id != 0xbc  // program_stream_map
			&& stream_id != 0xbe  // padding_stream
			&& stream_id != 0xbf  // private_stream_2
			&& stream_id != 0xf0  // ECM
			&& stream_id != 0xf1  // EMM
			&& stream_id != 0xff  // program_stream_directory
			&& stream_id != 0xf2  // DSMCC
			&& stream_id != 0xf8) {  // H.222.1 type E
		unsigned char pes_scrambling_control;
		unsigned char pes_priority;
		unsigned char data_alignment_indicator;
		unsigned char copyright;
		unsigned char original_or_copy;
		unsigned char pts_dts_flags;
		unsigned char escr_flag;
		unsigned char es_rate_flag;
		unsigned char dsm_trick_mode_flag;
		unsigned char additional_copy_info_flag;
		unsigned char pes_crc_flag;
		unsigned char pes_extension_flag;
		unsigned char pes_header_data_length;
		
		unsigned char pts_msb, dts_msb;
		unsigned int pts, dts;
		
		unsigned int data_length, data_start;
		
		if ( ((buf[consumed_bytes] & 0xC0) >> 6) != 0x02) {
			//printf("OOPS\n");
			return -1;
		}
		
		pes_scrambling_control = ((buf[consumed_bytes] & 0x30) >> 4);
		pes_priority = ((buf[consumed_bytes] & 0x08) >> 3);
		data_alignment_indicator = ((buf[consumed_bytes] & 0x04) >> 2);
		copyright = ((buf[consumed_bytes] & 0x02) >> 1);
		original_or_copy = (buf[consumed_bytes] & 0x01);
		
		pts_dts_flags = ((buf[consumed_bytes + 1] & 0xC0) >> 6);
		escr_flag = ((buf[consumed_bytes + 1] & 0x20) >> 5);
		es_rate_flag = ((buf[consumed_bytes + 1] & 0x10) >> 4);
		dsm_trick_mode_flag = ((buf[consumed_bytes + 1] & 0x08) >> 3);
		additional_copy_info_flag = ((buf[consumed_bytes + 1] & 0x04) >> 2);
		pes_crc_flag = ((buf[consumed_bytes + 1] & 0x02) >> 1);
		pes_extension_flag = (buf[consumed_bytes + 1] & 0x01);
		
		pes_header_data_length = buf[consumed_bytes + 2];
		
		consumed_bytes += 3;
		
		if (pts_dts_flags == 0x02) {
			if (((buf[consumed_bytes] & 0xF0) >> 4) != 0x02) {
				return -1;
			}
			
			if ((buf[consumed_bytes] & 0x01) != 1 ||
					(buf[consumed_bytes + 2] & 0x01) != 1 ||
					(buf[consumed_bytes + 4] & 0x01) != 1) {
				return -1;
			}
			
			pts_msb = ((buf[consumed_bytes] & 0x08) >> 3);
			pts = ((buf[consumed_bytes] & 0x06) << 29) | (buf[consumed_bytes + 1] << 22) | 
				((buf[consumed_bytes + 2] & 0xFE) << 14) | (buf[consumed_bytes + 3] << 7) | ((buf[consumed_bytes+4] & 0xFE) >> 1);
				
			consumed_bytes += 5;
		}
		
		if (pts_dts_flags == 0x03) {
			if (((buf[consumed_bytes] & 0xF0) >> 4) != 0x03) {
				return -1;
			}
			
			if (((buf[consumed_bytes + 5] & 0xF0) >> 4) != 0x01) {
				return -1;
			}
			
			if ((buf[consumed_bytes] & 0x01) != 1 ||
					(buf[consumed_bytes + 2] & 0x01) != 1 ||
					(buf[consumed_bytes + 4] & 0x01) != 1) {
				return -1;
			}
			
			if ((buf[consumed_bytes + 5] & 0x01) != 1 ||
					(buf[consumed_bytes + 7] & 0x01) != 1 ||
					(buf[consumed_bytes + 9] & 0x01) != 1) {
				return -1;
			}
			
			pts_msb = ((buf[consumed_bytes] & 0x08) >> 3);
			pts = ((buf[consumed_bytes] & 0x06) << 29) | (buf[consumed_bytes + 1] << 22) | 
				((buf[consumed_bytes + 2] & 0xFE) << 14) | (buf[consumed_bytes + 3] << 7) | ((buf[consumed_bytes+4] & 0xFE) >> 1);
				
			dts_msb = ((buf[consumed_bytes + 5] & 0x08) >> 3);
			dts = ((buf[consumed_bytes + 5] & 0x06) << 29) | (buf[consumed_bytes + 6] << 22) | 
				((buf[consumed_bytes + 7] & 0xFE) << 14) | (buf[consumed_bytes + 8] << 7) | ((buf[consumed_bytes+9] & 0xFE) >> 1);
				
			consumed_bytes += 10;
		}
		
		if (escr_flag == 0x01) {	
			consumed_bytes += 6;
		}
		
		if (es_rate_flag == 0x01) {
			consumed_bytes += 3;
		}
		
		if (dsm_trick_mode_flag == 0x01) {
			
		}
		
		if (additional_copy_info_flag == 0x01) {
			
		}
		
		if (pes_crc_flag == 0x01) {
			
		}
		
		if (pes_extension_flag == 0x01) {
			
		}
		
		data_start = 6 + 3 + pes_header_data_length;
		if (packet_length == 0) {
			data_length = len - data_start;
			//write(1, buf+data_start, data_length);
		} else {
			data_length = packet_length - 3 - pes_header_data_length;
			//write(1, buf+data_start, data_length);
		}
		
		if (pid == video_pid) {
			if (packet_length > 170) data_length = len - data_start;
			queue_video_buf(buf+data_start, data_length, pts);
			//printf("video data_length = %d\n", data_length);
			//printf("video pts_msb=%d video pts=%d dts_msb=%d dts=%d\n", pts_msb, pts, dts_msb, dts);
			return data_length;
		} else if (pid == audio_pid) {
			//printf("audio pts_msb=%d audio pts=%d dts_msb=%d dts=%d\n", pts_msb, pts, dts_msb, dts);			
			queue_audio_buf(buf+data_start, data_length, pts);
		}
	} else {
		/* PES_packet_data_byte or padding_byte */
	}
           
	//printf("parse pes 0x%08x 0x%02x 0x%04x\n", start_code_prefix, stream_id, packet_length);
	return 0;
}

/*
 * return consumed bytes
 */ 
int parse_adaptation_field(unsigned char *buf, int len)
{
	unsigned char adaptation_field_length;
	
	adaptation_field_length = buf[0];
	if (adaptation_field_length > 0) {
		/* Do nothing here. */
	}
	
	//printf("parse adaptation field: length = %d\n", adaptation_field_length);
	return adaptation_field_length + 1 /* adaptation_field_length */;
}

int parse_pmt(unsigned char *buf, int len)
{
	unsigned char table_id;
	unsigned char section_syntax_indicator;
	unsigned char reserved1;
	unsigned short section_length;
	unsigned short program_number;
	unsigned char reserved2;
	unsigned char version_number;
	unsigned char current_next_indicator;
	unsigned char section_number;
	unsigned char last_section_number;
	unsigned char reserved3;
	unsigned short pcr_pid;
	unsigned char reserved4;
	unsigned short program_info_length;
	unsigned int crc;
	
	short remaining_section, comuming_section;
	
	table_id = buf[0];
	if (table_id != 0x02) { /* It shoud be equal 0x02 */
		return -1;
	}
	
	section_syntax_indicator = (buf[1] & 0x80) >> 7;
	if (section_syntax_indicator != 1) { /* This bit should be 1 */
		return -1;
	}
	
	if ((buf[1] & 0x40) != 0) { /* This bit must be zero */
		return -1;
	}
	
	reserved1 = (buf[1] & 0x30) >> 4;
	
	section_length = ((buf[1] & 0x0F) << 8) | buf[2];
	
	if (section_length != len - 3) {
		/* OOPS */
	}
	
	program_number = (buf[3] << 8) | buf[4];
	
	reserved2 = (buf[5] & 0xC0) >> 6;
	version_number = (buf[5] & 0x1E) >> 1;
	current_next_indicator = buf[5] & 0x01;
	section_number = buf[6];
	last_section_number = buf[7];
	reserved3 = (buf[8] & 0xE0) >> 5;
	pcr_pid = ((buf[8] & 0x1F) << 8) | buf[9];
	reserved4 = (buf[10] & 0xF0) >> 4;
	program_info_length = ((buf[10] & 0x0F) << 8) | buf[11];
	
	remaining_section = section_length - 9 - program_info_length - 4;
	comuming_section = 0;
	
	while (remaining_section > 0) {
		unsigned char stream_type;
		unsigned short pid;
		unsigned short es_info_length;
		
		stream_type = buf[12+program_info_length+comuming_section];
		pid = ((buf[13+program_info_length+comuming_section] & 0x1F) << 8) | buf[14+program_info_length+comuming_section];
		es_info_length = ((buf[15+program_info_length+comuming_section] & 0x0F) << 8) | buf[16+program_info_length+comuming_section];
		comuming_section += comuming_section + (5 + es_info_length);
		remaining_section -= (5 + es_info_length);
		if (stream_type == 0x1b) { /* AVC Video */
			video_pid = pid;
		} else if(stream_type == 0x0f || stream_type == 0x83 || stream_type == 0x81) { /* Audio */
			audio_pid = pid;
			if (stream_type == 0x0f)
				audio_type = AAC;
			else if (stream_type == 0x83)
				audio_type = LPCM;
			else if (stream_type == 0x081)
				audio_type = AC3;
		}
	}
	
	if (video_pid == 0 & audio_pid == 0) {
		/* There is no valid audio or video data. */
	}
	
	//printf("parse PMT: Video PID = 0x%02X, Audio PID = 0x%02X\n", video_pid, audio_pid);
	
	return 0;
}

int parse_pat(unsigned char *buf, int len)
{
	unsigned char table_id;
	unsigned char section_syntax_indicator;
	unsigned char reserved1;
	unsigned short section_length;
	unsigned short transport_stream_id;
	unsigned char reserved2;
	unsigned char version_number;
	unsigned char current_next_indicator;
	unsigned char section_number;
	unsigned char last_section_number;
	unsigned int crc;
	
	int prog_no, i;

	table_id = buf[0];
	if (table_id != 0x00) { /* It shoud be equal 0x00 */
		return -1;
	}
	
	section_syntax_indicator = (buf[1] & 0x80) >> 7;
	if (section_syntax_indicator != 1) { /* This bit should be 1 */
		return -1;
	}
	
	if ((buf[1] & 0x40) != 0) { /* This bit must be zero */
		return -1;
	}
	
	reserved1 = (buf[1] & 0x30) >> 4;
	
	section_length = ((buf[1] & 0x0F) << 8) | buf[2];
	
	if (section_length != len - 3) {
		/* OOPS */
	}
	
	transport_stream_id = (buf[3] << 8) | buf[4];
	
	reserved2 = (buf[5] & 0xC0) >> 6;
	version_number = (buf[5] & 0x3E) >> 1;
	current_next_indicator = buf[5] & 0x01;
	section_number = buf[6];
	last_section_number = buf[7];
	
	prog_no = (section_length - 5 - 4 /* CRC Length */) / 4;
	
	if (prog_no <= 0) return -1;
	
	for (i=0 ; i<prog_no ; i++) {
		unsigned short program_number;
		unsigned pid;
		
		program_number = (buf[8+4*i] << 8) | buf[9+4*i];
		
		if (program_number == 0) {
			pid = ((buf[10+4*i] & 0x1F) << 8 | buf[11+4*i]);
		} else {
			pid = ((buf[10+4*i] & 0x1F) << 8 | buf[11+4*i]);
			pmt_pid = pid;
		} 
	}
	
	crc = (buf[8+4*prog_no] << 24) | (buf[9+4*prog_no] << 16) | (buf[10+4*prog_no] << 8) | (buf[11+4*prog_no]);
	
	
	//printf("parse PAT: PMT PID = 0x%02X\n", pmt_pid);
	return 0;
}

int parse_pid(unsigned char *buf, int len, int pid, int payload_unit_start_indicator, unsigned int *time)
{
	//printf("parse pid 0x%02x\n", pid);
	int ret = 0;
	
	if (pid == 0) {
		int offset = 0;
		if (payload_unit_start_indicator) {
			offset = buf[0];
		}
		
		return parse_pat(buf+offset+1, len-offset-1);
	}
	
	if (pmt_pid == 0) {
		/* Did not get program map */
		return -1;
	}
	
	if (pid == pmt_pid) {
		int offset = 0;
		if (payload_unit_start_indicator) {
			offset = buf[0];
		}
		
		return parse_pmt(buf+offset+1, len-offset-1);
	}
	
	if (pid == video_pid) {		
		if (payload_unit_start_indicator) {
			/* flush gathered data */
			parse_pes(video_buf, video_buf_len, pid, time);
			//printf("audio_buf_len = %d\n", audio_buf_len);
			video_buf_len = 0;
		}
		
		if (video_buf_len+len > VIDEO_BUF_CAP) printf("OOPS %d\n", video_buf_len);
		else memcpy(video_buf+video_buf_len, buf, len);
		video_buf_len += len;

	} else if (pid == audio_pid) {
		/* gather audio payload */
		if (payload_unit_start_indicator) {
			/* flush gathered data */
			parse_pes(audio_buf, audio_buf_len, pid, time);
			//printf("audio_buf_len = %d\n", audio_buf_len);
			audio_buf_len = 0;
		}
		
		if (audio_buf_len+len > AUDIO_BUF_CAP) printf("OOPS %d\n", audio_buf_len);
		else memcpy(audio_buf+audio_buf_len, buf, len);
		audio_buf_len += len;
	} else {
		/* unknow pid */
	}
	
	return ret;
}

int parse_ts_packet(unsigned char *buf, int len)
{
	unsigned char transport_error_indicator;
	unsigned char payload_unit_start_indicator;
	unsigned char transport_priority;
	unsigned short pid;
	unsigned char transport_scrambling_control;
	unsigned char adaptation_field_control;
	unsigned char cc;
	
	unsigned int indicator, time;
	
	int offset;
	
	if (buf[0] != SYNC_BYTE) {
		return -1;
	}
	
	transport_error_indicator = (buf[1] & 0x80) >> 7;
	payload_unit_start_indicator = (buf[1] & 0x40) >> 6;
	transport_priority = (buf[1] & 0x20) >> 5;
	pid = ((buf[1] & 0x1F) << 8) | buf[2];
	transport_scrambling_control = (buf[3] & 0xC0) >> 6;
	adaptation_field_control = (buf[3] & 0x30) >> 4;
	cc = buf[3] & 0x0F;
	
	offset = 4;
	
	if (adaptation_field_control == 2 || adaptation_field_control == 3) {
		offset += parse_adaptation_field(buf+offset, len-offset);
	}
	
	if (adaptation_field_control == 1 || adaptation_field_control == 3) {
		if (pid == video_pid)
			indicator = payload_unit_start_indicator;
		return parse_pid(buf+offset, len-offset, pid, payload_unit_start_indicator, &time);
	}
	
	return 0;
}
