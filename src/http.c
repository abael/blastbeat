#include "../blastbeat.h"

int bb_manage_chunk(struct bb_session_request *bbsr, char *buf, size_t len) {
	struct bb_session *bbs = bbsr->bbs;
	char *chunk = malloc(MAX_CHUNK_STORAGE);
        if (!chunk) {
        	bb_error("unable to allocate memory for chunked response: malloc()");
                bb_session_close(bbs);
		return -1;
        }
        int chunk_len = snprintf(chunk, MAX_CHUNK_STORAGE, "%X\r\n", (unsigned int) len);

        if (bb_wq_push(bbs->connection, chunk, chunk_len, 1)) goto end;
        if (bb_wq_push_copy(bbs->connection, buf, len, 1)) goto end;
        if (bb_wq_push(bbs->connection, "\r\n", 2, 0)) goto end;
        if (len == 0 && bbsr->close) {
        	if (bb_wq_push_close(bbs->connection)) goto end;
	}
	return 0;
end:
	bb_session_close(bbs);
	return -1;
}

static int url_cb(http_parser *parser, const char *buf, size_t len) {
        struct bb_session_request *bbsr = (struct bb_session_request *) parser->data;
        if (!bbsr->headers[0].key) {
                bbsr->headers[0].key = malloc(len);
                memcpy(bbsr->headers[0].key, buf, len);
                bbsr->headers[0].keylen = len;
        }
        else {
                bbsr->headers[0].key = realloc(bbsr->headers[0].key, bbsr->headers[0].keylen + len);
                memcpy(bbsr->headers[0].key + bbsr->headers[0].keylen + len, buf, len);
                bbsr->headers[0].keylen += len;
        }
        return 0;
}


static int null_cb(http_parser *parser) {
        return 0;
}

static int null_b_cb(http_parser *parser, const char *buf, size_t len) {
        return 0;
}

static int header_field_cb(http_parser *parser, const char *buf, size_t len) {
        struct bb_session_request *bbsr = (struct bb_session_request *) parser->data;
        if (bbsr->last_was_value) {
                bbsr->header_pos++;
                bbsr->headers[bbsr->header_pos].key = malloc(len);
                memcpy(bbsr->headers[bbsr->header_pos].key, buf, len);
                bbsr->headers[bbsr->header_pos].keylen = len;
        }
        else {
                bbsr->headers[bbsr->header_pos].key = realloc(bbsr->headers[bbsr->header_pos].key, bbsr->headers[bbsr->header_pos].keylen + len);
                memcpy(bbsr->headers[bbsr->header_pos].key + bbsr->headers[bbsr->header_pos].keylen, buf, len);
                bbsr->headers[bbsr->header_pos].keylen += len;
        }
        bbsr->last_was_value = 0;
        return 0;
}

static int header_value_cb(http_parser *parser, const char *buf, size_t len) {
        struct bb_session_request *bbsr = (struct bb_session_request *) parser->data;
        if (!bbsr->last_was_value) {
                bbsr->headers[bbsr->header_pos].value = malloc(len);
                memcpy(bbsr->headers[bbsr->header_pos].value, buf, len);
                bbsr->headers[bbsr->header_pos].vallen = len;
        }
        else {
                bbsr->headers[bbsr->header_pos].value = realloc(bbsr->headers[bbsr->header_pos].value, bbsr->headers[bbsr->header_pos].vallen + len);
                memcpy(bbsr->headers[bbsr->header_pos].value + bbsr->headers[bbsr->header_pos].vallen, buf, len);
                bbsr->headers[bbsr->header_pos].vallen += len;
        }
        bbsr->last_was_value = 1;
        return 0;
}

static int body_cb(http_parser *parser, const char *buf, size_t len) {
        struct bb_session_request *bbsr = (struct bb_session_request *) parser->data;
        // send a message as "body"
	if (bbsr->sio_post) {
		char *new_buf = realloc(bbsr->sio_post_buf, bbsr->sio_post_buf_size+len);
		if (!new_buf) {
			bb_error("realloc()");
			return -1;
		}
		bbsr->sio_post_buf = new_buf;
		memcpy(bbsr->sio_post_buf+bbsr->sio_post_buf_size, buf, len);
		bbsr->sio_post_buf_size+=len;
	}
	else {
		bb_zmq_send_msg(bbsr->bbs->dealer->identity, bbsr->bbs->dealer->len, (char *) &bbsr->bbs->uuid_part1, BB_UUID_LEN, "body", 4, (char *) buf, len);
	}
        return 0;
}

static int response_headers_complete(http_parser *parser) {
        struct bb_session_request *bbsr = (struct bb_session_request *) parser->data;
        if (!http_should_keep_alive(parser)) {
                bbsr->close = 1;
        }
        if (parser->content_length != ULLONG_MAX) {
                bbsr->content_length = parser->content_length;
        }
        return 0;
}

static int bb_session_headers_complete(http_parser *parser) {
        struct bb_session_request *bbsr = (struct bb_session_request *) parser->data;

        // ok get the Host header
        struct bb_http_header *bbhh = bb_http_req_header(bbsr, "Host", 4);
        if (!bbhh) {
                return -1;
        }

        if (!bbsr->bbs->dealer) {
                if (bb_set_dealer(bbsr->bbs, bbhh->value, bbhh->vallen)) {
                	return -1;
        	}
        }

	if (parser->content_length != ULLONG_MAX) {
                bbsr->content_length = parser->content_length;
        }

	// check for socket.io
	if (!bb_startswith(bbsr->headers[0].key, bbsr->headers[0].keylen, "/socket.io/1/", 13)) {
		if (bb_manage_socketio(bbsr)) {
			return -1;
		}
	}

        if (parser->upgrade) {
                struct bb_http_header *bbhh = bb_http_req_header(bbsr, "Upgrade", 7);
                if (bbhh) {
                        if (!bb_stricmp("websocket", 9, bbhh->value, bbhh->vallen)) {
                                bbsr->type = BLASTBEAT_TYPE_WEBSOCKET;
                                bb_send_websocket_handshake(bbsr);
				goto msg;
                        }
                }
        }


        if (!http_should_keep_alive(parser)) {
                //printf("NO KEEP ALIVE !!!\n");
                bbsr->close = 1;
        }

msg:
	if (bbsr->no_uwsgi) return 0;
        // now encode headers in a uwsgi packet and send it as "headers" message
	if (bb_uwsgi(bbsr)) {
		return -1;
	}
        bb_zmq_send_msg(bbsr->bbs->dealer->identity, bbsr->bbs->dealer->len, (char *) &bbsr->bbs->uuid_part1, BB_UUID_LEN, "uwsgi", 5, bbsr->uwsgi_buf, bbsr->uwsgi_pos);
        return 0;
}

static char *find_third_colon(char *buf, size_t len) {
	size_t i;
        int count = 0;
        for(i=0;i<len;i++) {
                if (buf[i] == ':') {
                        count++;
			if (count == 3) {
				if ((i+1) > (len-1)) return NULL;
				return buf+i+1;
			}
		}
        }
	return NULL;
} 

static size_t str2num(char *str, int len) {

        int i;
        size_t num = 0;

        size_t delta = pow(10, len);

        for (i = 0; i < len; i++) {
                delta = delta / 10;
                num += delta * (str[i] - 48);
        }

        return num;
}


int bb_socketio_message(struct bb_session *bbs, char *buf, size_t len) {
	char *sio_body = find_third_colon(buf, len);
        if (!sio_body) return -1;
        size_t sio_len = len - (sio_body-buf);
        // forward socket.io message to the right session
                	fprintf(stderr,"SOCKET.IO MESSAGE TYPE: %c\n", buf[0]);
        switch(buf[0]) {
        	case '3':
                	bb_zmq_send_msg(bbs->dealer->identity, bbs->dealer->len, (char *) &bbs->uuid_part1, BB_UUID_LEN, "socket.io/msg", 13, sio_body, sio_len);
			break;
                case '4':
                	bb_zmq_send_msg(bbs->dealer->identity, bbs->dealer->len, (char *) &bbs->uuid_part1, BB_UUID_LEN, "socket.io/json", 14, sio_body, sio_len);
                	break;
                case '5':
                	bb_zmq_send_msg(bbs->dealer->identity, bbs->dealer->len, (char *) &bbs->uuid_part1, BB_UUID_LEN, "socket.io/event", 15, sio_body, sio_len);
                        break;
		default:
                	fprintf(stderr,"SOCKET.IO MESSAGE TYPE: %c\n", buf[0]);
			return -1;
	}
	return 0;
}

static int bb_session_request_complete(http_parser *parser) {
        if (parser->upgrade) return 0;
	struct bb_session_request *bbsr = (struct bb_session_request *) parser->data;
	if (bbsr->sio_post) {
		// minimal = X:::
		if (bbsr->sio_post_buf_size < 4) return -1;
		// multipart message ?
		fprintf(stderr,"%01x %01x %01x %01x %x\n", bbsr->sio_post_buf[0], bbsr->sio_post_buf[1],bbsr->sio_post_buf[2],bbsr->sio_post_buf[3], bbsr->sio_post_buf[4]);
		if (bbsr->sio_post_buf[0] == '\xef' && bbsr->sio_post_buf[1] == '\xbf' && bbsr->sio_post_buf[2] == '\xbd') {
			fprintf(stderr,"MULTIPART MESSAGE\n");
			char *ptr = bbsr->sio_post_buf;
			char *watermark = ptr+bbsr->sio_post_buf_size;
			while(ptr < watermark) {
				if (*ptr++ != '\xef') return -1;	
				if (ptr+1 > watermark) return -1;
				if (*ptr++ != '\xbf') return -1;	
				if (ptr+1 > watermark) return -1;
				if (*ptr++ != '\xbd') return -1;	
				if (ptr+1 > watermark) return -1;
				char *base_of_num = ptr;
				size_t end_of_num = 0;
				while(*ptr >= '0' && *ptr<='9') {
					if (ptr+1 > watermark) return -1;
					end_of_num++;
					ptr++;
				}
				size_t part_len = str2num(base_of_num, end_of_num);
				fprintf(stderr,"msg part size = %d\n", part_len);
				if (*ptr++ != '\xef') return -1;	
				if (ptr+1 > watermark) return -1;
				if (*ptr++ != '\xbf') return -1;	
				if (ptr+1 > watermark) return -1;
				if (*ptr++ != '\xbd') return -1;	
				if (ptr+1 > watermark) return -1;
				if (ptr+part_len > watermark) return -1;
				if (bb_socketio_message(bbsr->sio_bbs, ptr, part_len))
					return -1;
				ptr+=part_len;
			}
		}
		else {
			if (bb_socketio_message(bbsr->sio_bbs, bbsr->sio_post_buf, bbsr->sio_post_buf_size))
				return -1;
		}
	} 
        if (http_should_keep_alive(parser)) {
                // prepare for a new request
                bbsr->bbs->new_request = 1;
        }
        return 0;
}


http_parser_settings bb_http_parser_settings = {
        .on_message_begin = null_cb,
        .on_message_complete = bb_session_request_complete,
        .on_headers_complete = bb_session_headers_complete,
        .on_header_field = header_field_cb,
        .on_header_value = header_value_cb,
        .on_url = url_cb,
        .on_body = body_cb,
};

http_parser_settings bb_http_response_parser_settings = {
        .on_message_begin = null_cb,
        .on_message_complete = null_cb,
        .on_headers_complete = response_headers_complete,
        .on_header_field = null_b_cb,
        .on_header_value = null_b_cb,
        .on_url = null_b_cb,
        .on_body = null_b_cb,
};

http_parser_settings bb_http_response_parser_settings2 = {
        .on_message_begin = null_cb,
        .on_message_complete = null_cb,
        .on_headers_complete = null_cb,
        .on_header_field = header_field_cb,
        .on_header_value = header_value_cb,
        .on_url = null_b_cb,
        .on_body = null_b_cb,
};


