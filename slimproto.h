/* 
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2021, ralph_irving@hotmail.com
 *  
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// packet formats for slimproto

#ifndef SUN
#pragma pack(push, 1)
#else
#pragma pack(1)
#endif

// from S:N:Slimproto _hello_handler
struct HELO_packet {
	char  opcode[4];
	u32_t length;
	u8_t  deviceid;
	u8_t  revision;
	u8_t  mac[6];
	u8_t  uuid[16];
	u16_t wlan_channellist;
	u32_t bytes_received_H, bytes_received_L;
	char  lang[2];
	//	u8_t capabilities[];
};

// S:N:Slimproto _stat_handler
struct STAT_packet {
	char  opcode[4];
	u32_t length;
	u32_t event;
	u8_t  num_crlf;
	u8_t  mas_initialized;
	u8_t  mas_mode;
	u32_t stream_buffer_size;
	u32_t stream_buffer_fullness;
	u32_t bytes_received_H;
	u32_t bytes_received_L;
	u16_t signal_strength;
	u32_t jiffies;
	u32_t output_buffer_size;
	u32_t output_buffer_fullness;
	u32_t elapsed_seconds;
	u16_t voltage;
	u32_t elapsed_milliseconds;
	u32_t server_timestamp;
	u16_t error_code;
};

// S:N:Slimproto _disco_handler
struct DSCO_packet {
	char  opcode[4];
	u32_t length;
	u8_t  reason;
};

// S:N:Slimproto _http_response_handler
struct RESP_header {
	char  opcode[4];
	u32_t length;
	// char header[] - added in sendRESP
};

// S:N:Slimproto _http_metadata_handler
struct META_header {
	char  opcode[4];
	u32_t length;
	// char metadata[]
};

// S:N:Slimproto _http_setting_handler
struct SETD_header {
	char  opcode[4];
	u32_t length;
	u8_t  id;
	// data
};

#if IR
struct IR_packet {
	char  opcode[4];
	u32_t length;
	u32_t jiffies;
	u8_t  format; // ignored by server
	u8_t  bits;   // ignored by server
	u32_t ir_code;
};
#endif

// from S:P:Squeezebox stream_s
struct strm_packet {
	char  opcode[4];
	char  command;
	u8_t  autostart;
	u8_t  format;
	u8_t  pcm_sample_size;
	u8_t  pcm_sample_rate;
	u8_t  pcm_channels;
	u8_t  pcm_endianness;
	u8_t  threshold;
	u8_t  spdif_enable;
	u8_t  transition_period;
	u8_t  transition_type;
	u8_t  flags;
	u8_t  output_threshold;
	u8_t  slaves;
	u32_t replay_gain;
	u16_t server_port;
	u32_t server_ip;
	//char request_string[];
};

// S:P:Squeezebox2
struct aude_packet {
	char  opcode[4];
	u8_t  enable_spdif;
	u8_t  enable_dac;
};

// S:P:Squeezebox2
struct audg_packet {
	char  opcode[4];
	u32_t old_gainL;     // unused
	u32_t old_gainR;     // unused
	u8_t  adjust;
	u8_t  preamp;        // unused
	u32_t gainL;
	u32_t gainR;
	// squence ids - unused
};

// S:P:Squeezebox2
struct cont_packet {
	char  opcode[4];
	u32_t metaint;
	u8_t  loop;
	// guids we don't use
};

// S:C:Commands
struct serv_packet {
	char  opcode[4];
	u32_t server_ip;
	// possible sync group
};

// S:P:Squeezebox2
struct setd_packet {
	char  opcode[4];
	u8_t  id;
	char  data[];
};

// codec open - this is an extension to slimproto to allow the server to read the header and then return decode params
struct codc_packet {
	char  opcode[4];
	u8_t  format;
	u8_t  pcm_sample_size;
	u8_t  pcm_sample_rate;
	u8_t  pcm_channels;
	u8_t  pcm_endianness;
};

#ifndef SUN
#pragma pack(pop)
#else
#pragma pack()
#endif
