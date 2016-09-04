#include "FLAC/all.h"

#include <nan.h>
#include <cassert>
#include <cstring>
#include <iostream>

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

FLAC__StreamDecoder* decoder = nullptr;
bool ended = false;
bool packetProcessed = false;
char* bufferData = nullptr;
size_t bufferLength = 0;
size_t bufferPosition = 0;

FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder* decoder, const FLAC__Frame* frame, const FLAC__int32 * const buffer[], void *client_data) {
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;

	// ClientData *cd = (ClientData*)client_data;
	//
	// (void)buffer;
	// FLAC__ASSERT(0 != cd);
	//
	// if(!cd->error_occurred) {
	// 	const unsigned blocksize = frame->header.blocksize;
	// 	const FLAC__uint64 frame_first_sample = cd->samples_written;
	// 	const FLAC__uint64 frame_last_sample = frame_first_sample + (FLAC__uint64)blocksize - 1;
	// 	FLAC__uint64 test_sample;
	// 	unsigned i;
	// 	for(i = cd->first_seekpoint_to_check; i < cd->seektable_template->num_points; i++) {
	// 		test_sample = cd->seektable_template->points[i].sample_number;
	// 		if(test_sample > frame_last_sample) {
	// 			break;
	// 		}
	// 		else if(test_sample >= frame_first_sample) {
	// 			cd->seektable_template->points[i].sample_number = frame_first_sample;
	// 			cd->seektable_template->points[i].stream_offset = cd->last_offset - cd->audio_offset;
	// 			cd->seektable_template->points[i].frame_samples = blocksize;
	// 			cd->first_seekpoint_to_check++;
	// 			/* DO NOT: "break;" and here's why:
	// 			 * The seektable template may contain more than one target
	// 			 * sample for any given frame; we will keep looping, generating
	// 			 * duplicate seekpoints for them, and we'll clean it up later,
	// 			 * just before writing the seektable back to the metadata.
	// 			 */
	// 		}
	// 		else {
	// 			cd->first_seekpoint_to_check++;
	// 		}
	// 	}
	// 	cd->samples_written += blocksize;
	// 	if(!FLAC__stream_decoder_get_decode_position(decoder, &cd->last_offset))
	// 		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	// 	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
	// }
	// else
	// 	return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
}

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

FLAC__StreamDecoderReadStatus read_callback(const FLAC__StreamDecoder* decoder, FLAC__byte buffer[], size_t* bytes, void* client_data) {
	if (ended)
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;

	std::cout << "READ" << std::endl;
	std::cout << "bufferPosition: " << bufferPosition << std::endl;
	std::cout << "bufferLength: " << bufferLength << std::endl;
	std::cout << "(*bytes): " << (*bytes) << std::endl;

	size_t length = bufferLength - bufferPosition;
	length = length > *bytes ? *bytes : length;
	std::cout << "length: " << length << std::endl;

	if (length == 0) {
		*bytes = 0;
		packetProcessed = true;
		return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
	}

	if (*bytes > 0) {
		std::cout << "bufferPosition + length <= bufferLength: " << (bufferPosition + length) << " <= " << bufferLength << std::endl;
		if (bufferPosition + length <= bufferLength) {
			memcpy(buffer, bufferData + bufferPosition, length);
			bufferPosition += length;

			if (bufferPosition == bufferLength) {
				std::cout << "setting done flag!" << std::endl;
				packetProcessed = true;
			}
		}
		else {
			*bytes = 0;
		}
		return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
	}

	return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
}

NAN_METHOD(init) {
	// TODO: init new stream structures
	std::cout << "[+] init" << std::endl;
	decoder = FLAC__stream_decoder_new();

	FLAC__StreamDecoderInitStatus status = FLAC__stream_decoder_init_stream(
		decoder,
		read_callback,
		nullptr, // FLAC__StreamDecoderSeekCallback seek_callback,
		nullptr, // FLAC__StreamDecoderTellCallback tell_callback,
		nullptr, // FLAC__StreamDecoderLengthCallback length_callback,
		nullptr, // FLAC__StreamDecoderEofCallback eof_callback,
		write_callback,
		nullptr, // FLAC__StreamDecoderMetadataCallback metadata_callback,
		error_callback,
		nullptr); // client data

	assert(status == FLAC__STREAM_DECODER_INIT_STATUS_OK);
}

NAN_METHOD(process_packet) {
	if (info.Length() != 1) {
		Nan::ThrowError("process_packet: requires 1 argument: buffer");
		return;
	}

	Local<Object> bufferObj = info[0]->ToObject();
	bufferLength = node::Buffer::Length(bufferObj);
	bufferData = node::Buffer::Data(bufferObj);
	if (bufferLength == 0 || !bufferData) {
		return;
	}


	std::cout << "begin process_single: " << bufferLength << std::endl;
	size_t counter = 0;
	bufferPosition = 0;
	packetProcessed = false;
	while (FLAC__stream_decoder_process_single(decoder)) {
		std::cout << "#" << counter << ": pos = " << bufferPosition << std::endl;
		counter++;
		if (packetProcessed) {
			std::cout << "bailing out" << std::endl;
			break;
		}
	}
	std::cout << "[+] process_queue: " << counter << " times" << std::endl;

	bufferData = nullptr;
	bufferLength = 0;
}

// NAN_METHOD(process_queue) {
// 	FLAC__bool ret = FLAC__stream_decoder_process_single(decoder);
// 	std::cout << "[+] process_queue:" << ret << std::endl;
// 	info.GetReturnValue().Set(true);
// }

NAN_METHOD(end) {
	// TODO: let the decoder know this is it
	ended = true;
	std::cout << "[+] end" << std::endl;
}

NAN_METHOD(get_table) {
	std::cout << "[+] get_table" << std::endl;
	info.GetReturnValue().Set(1887);
}

NAN_METHOD(clear) {
	// TODO: clean up any allocated flac structures
	std::cout << "[+] clear" << std::endl;

	FLAC__stream_decoder_delete(decoder);
	decoder = nullptr;
	ended = false;
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
	// NAN_EXPORT(target, process_queue);
	NAN_EXPORT(target, end);
	NAN_EXPORT(target, get_table);
	NAN_EXPORT(target, clear);

	std::cout << "[+] init" << std::endl;
}

NODE_MODULE(flacseektable, initialize)

}
