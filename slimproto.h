/* 
 *  Squeezelite - lightweight headless squeezeplay emulator for linux
 *
 *  (c) Adrian Smith 2012, triode1@btinternet.com
 *  
 *  Unreleased - license details to be added here...
 */

// packet formats for slimproto

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
} __attribute__((packed));

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
} __attribute__((packed));

// S:N:Slimproto _disco_handler
struct DSCO_packet {
	char  opcode[4];
	u32_t length;
	u8_t  reason;
} __attribute__((packed));

// S:N:Slimproto _http_response_handler
struct RESP_header {
	char  opcode[4];
	u32_t length;
	// char header[] - added in sendRESP
} __attribute__((packed));

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
} __attribute__((packed));

// S:P:Squeezebox2
struct audg_packet {
	char  opcode[4];
	u32_t old_gainL;     // unused
	u32_t old_gainR;     // unused
	u8_t  fixed_digital; // unused
	u8_t  preamp;        // unused
	u32_t gainL;
	u32_t gainR;
	// squence ids - unused
} __attribute__((packed));

