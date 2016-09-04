#include "FLAC/all.h"

#include <nan.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <queue>
#include <string>

namespace flacseektable {

using node::AtExit;

using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::String;
using v8::Value;

using Nan::GetFunction;
using Nan::New;
using Nan::Set;

typedef std::queue< std::string > BufferQueue;

struct ClientData {
	BufferQueue buffers;
	const char* data;
	size_t length;
	size_t position;
	size_t absolutePosition;
	bool end;
	FLAC__StreamMetadata* seekBlock;
	FLAC__uint64 audioOffset;
	FLAC__uint64 totalSamples;
	unsigned sampleRate;
};

FLAC__StreamDecoder* decoder = nullptr;
ClientData* clientData = nullptr;

void error_callback(const FLAC__StreamDecoder* decoder, FLAC__StreamDecoderErrorStatus status, void* client_data) {
	std::cout << "FLAC ERROR: " << status << std::endl;
	std::cout << "\t";
	switch (status) {
		case FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC:
			std::cout << "An error in the stream caused the decoder to lose synchronization.";
			break;
		case FLAC__STREAM_DECODER_ERROR_STATUS_BAD_HEADER:
			std::cout << "The decoder encountered a corrupted frame header.";
			break;
		case FLAC__STREAM_DECODER_ERROR_STATUS_FRAME_CRC_MISMATCH:
			std::cout << "The frame's data did not match the CRC in the footer.";
			break;
		case FLAC__STREAM_DECODER_ERROR_STATUS_UNPARSEABLE_STREAM:
			std::cout << "The decoder encountered reserved fields in use in the stream.";
			break;
		default:
			std::cout << "Unknown error";
			break;
	}
	std::cout << std::endl;
}

bool gen_template(const FLAC__StreamDecoder* decoder, ClientData* cd) {
	assert(cd);
	if (cd->totalSamples == 0)
		return true;
	assert(cd->sampleRate > 0);
	unsigned samples = (unsigned)(1.0 * (double)cd->sampleRate);
	/* Restrict seekpoints to two per second of audio. */
	samples = samples < cd->sampleRate / 2 ? cd->sampleRate / 2 : samples;
	if (samples > 0) {
		/* +1 for the initial point at sample 0 */
		if (!FLAC__metadata_object_seektable_template_append_spaced_points_by_samples(cd->seekBlock, samples, cd->totalSamples))
			return false;
	}
	return true;
}

void metadata_callback(const FLAC__StreamDecoder* decoder, const FLAC__StreamMetadata* metadata, void* client_data) {
	ClientData* cd = (ClientData*)client_data;

	std::cout << "METADATA received:" << std::endl;

	switch (metadata->type) {
		case FLAC__METADATA_TYPE_STREAMINFO:
			cd->sampleRate = metadata->data.stream_info.sample_rate;
			cd->totalSamples = metadata->data.stream_info.total_samples;
			std::cout << "\ttype: STREAMINFO block" << std::endl;
			std::cout << "\tsample_rate: " << cd->sampleRate << std::endl;
			std::cout << "\ttotal_samples: " << cd->totalSamples << std::endl;
			break;
		case FLAC__METADATA_TYPE_PADDING:
			std::cout << "\ttype: PADDING block" << std::endl;
			break;
		case FLAC__METADATA_TYPE_APPLICATION:
			std::cout << "\ttype: APPLICATION block" << std::endl;
			break;
		case FLAC__METADATA_TYPE_SEEKTABLE:
			std::cout << "\ttype: SEEKTABLE block" << std::endl;
			break;
		case FLAC__METADATA_TYPE_VORBIS_COMMENT:
			std::cout << "\ttype: VORBISCOMMENT block (a.k.a. FLAC tags)" << std::endl;
			break;
		case FLAC__METADATA_TYPE_CUESHEET:
			std::cout << "\ttype: CUESHEET block" << std::endl;
			break;
		case FLAC__METADATA_TYPE_PICTURE:
			std::cout << "\ttype: PICTURE block" << std::endl;
			break;
		case FLAC__METADATA_TYPE_UNDEFINED:
			std::cout << "\ttype: UNDEFINED block" << std::endl;
			break;
		default:
			std::cout << "\ttype: ERROR: not parsed" << std::endl;
			break;
	}

	std::cout << "\tis last: " << metadata->is_last << std::endl;
	std::cout << "\tlength: " << metadata->length << std::endl;

	if (metadata->is_last) {
		FLAC__stream_decoder_get_decode_position(decoder, &cd->audioOffset);
		std::cout << "AUDIO offset: " << cd->audioOffset << std::endl;
		cd->seekBlock = FLAC__metadata_object_new(FLAC__METADATA_TYPE_SEEKTABLE);
		if (!gen_template(decoder, cd)) {
			std::cout << "ERROR: gen_template failed" << std::endl;
		}
	}
}

FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder* decoder, const FLAC__Frame* frame, const FLAC__int32 * const buffer[], void *client_data) {
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

FLAC__StreamDecoderTellStatus tell_callback(const FLAC__StreamDecoder* decoder, FLAC__uint64* absolute_byte_offset, void* client_data) {
	ClientData* cd = (ClientData*)client_data;
	*absolute_byte_offset = cd->absolutePosition;
	return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderReadStatus read_callback(const FLAC__StreamDecoder* decoder, FLAC__byte buffer[], size_t* bytes, void* client_data) {
	ClientData* cd = (ClientData*)client_data;

	// Allocation error has occurred
	if (*bytes == 0) {
		return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
	}

	if (cd->buffers.size() == 0 && cd->end) {
		// std::cout << "==== END OF STREAM (top) ====" << std::endl;
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}

	// std::cout << "==== READ CALLBACK: buffer count: " << cd->buffers.size() << std::endl;
	// std::cout << "decode buffer size: " << (*bytes) << std::endl;

	if (!cd->data) {
		std::string& buf = cd->buffers.front();
		cd->position = 0;
		cd->data = buf.data();
		cd->length = buf.length();
	}

	size_t chunkLength = cd->length - cd->position;
	// std::cout << "remaining: " << chunkLength << std::endl;
	chunkLength = chunkLength > *bytes ? *bytes : chunkLength;

	// std::cout << "chunkLength: " << chunkLength << std::endl;
	// std::cout << "length: " << cd->length << std::endl;
	// std::cout << "position: " << cd->position << std::endl;

	memcpy(buffer, cd->data + cd->position, chunkLength);
	cd->position += chunkLength;
	cd->absolutePosition += chunkLength;
	*bytes = chunkLength;

	// When at the end of the first buffer, switch to the next one
	if (cd->position == cd->length) {
		// std::cout << "==== MOVING TO NEXT BUFFER" << std::endl;
		cd->position = 0;
		cd->length = 0;
		cd->data = nullptr;
		cd->buffers.pop();
	}

	if (cd->buffers.size() == 0 && cd->end) {
		// std::cout << "==== END OF STREAM (bottom) ====" << std::endl;
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}

	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

void process_buffers() {
	while (1) {
		// FLAC__uint64 pos = 0;
		// FLAC__stream_decoder_get_decode_position(decoder, &pos);
		// std::cout << "decoder pos: " << pos << std::endl;
		FLAC__bool status = FLAC__stream_decoder_process_single(decoder);
		// std::cout << "state: " << FLAC__stream_decoder_get_state(decoder) << " " << FLAC__stream_decoder_get_resolved_state_string(decoder) << std::endl;
		if (!status) {
			break;
		}

		if (!clientData->end && clientData->buffers.size() < 2) {
			// std::cout << "process_buffers: returning to get more data" << std::endl;
			break;
		}
	}
}

NAN_METHOD(process_packet) {
	if (info.Length() != 1) {
		Nan::ThrowError("process_packet: requires 1 argument: buffer");
		return;
	}

	// std::cout << "==== process_packet(chunk) ====" << std::endl;

	Local<Object> bufferObj = info[0]->ToObject();
	size_t bufferLength = node::Buffer::Length(bufferObj);
	char* bufferData = node::Buffer::Data(bufferObj);
	if (bufferLength == 0 || !bufferData) {
		return;
	}

	clientData->buffers.push(std::string(bufferData, bufferLength));
	process_buffers();
}

NAN_METHOD(init) {
	std::cout << "[+] init" << std::endl;

	decoder = FLAC__stream_decoder_new();
	FLAC__stream_decoder_set_metadata_respond_all(decoder);

	clientData = new ClientData();
	clientData->data = nullptr;
	clientData->length = 0;
	clientData->position = 0;
	clientData->absolutePosition = 0;
	clientData->end = false;
	clientData->seekBlock = nullptr;
	clientData->totalSamples = 0;
	clientData->sampleRate = 0;

	FLAC__StreamDecoderInitStatus status = FLAC__stream_decoder_init_stream(
		decoder,
		read_callback,
		nullptr, // FLAC__StreamDecoderSeekCallback seek_callback,
		tell_callback,
		nullptr, // FLAC__StreamDecoderLengthCallback length_callback,
		nullptr, // FLAC__StreamDecoderEofCallback eof_callback,
		write_callback,
		metadata_callback,
		error_callback,
		clientData); // client data

	assert(status == FLAC__STREAM_DECODER_INIT_STATUS_OK);
}

NAN_METHOD(end) {
	clientData->end = true;
	// let the decoder know this is it
	process_buffers();

	std::cout << "[+] end" << std::endl;
}

NAN_METHOD(get_table) {
	std::cout << "[+] get_table" << std::endl;
	info.GetReturnValue().Set(1887);
}

NAN_METHOD(clear) {
	std::cout << "[+] clear" << std::endl;

	// clean up
	FLAC__metadata_object_delete(clientData->seekBlock);
	FLAC__stream_decoder_delete(decoder);
	decoder = nullptr;
	delete clientData;
	clientData = nullptr;
}

void shutdown(void*) {
	if (decoder) {
		FLAC__stream_decoder_delete(decoder);
		decoder = nullptr;
	}
	std::cout << "[+] shutdown" << std::endl;
}

NAN_MODULE_INIT(initialize) {
	AtExit(shutdown);

	NAN_EXPORT(target, init);
	NAN_EXPORT(target, process_packet);
	NAN_EXPORT(target, end);
	NAN_EXPORT(target, get_table);
	NAN_EXPORT(target, clear);

	std::cout << "[+] init" << std::endl;
}

NODE_MODULE(flacseektable, initialize)

}
