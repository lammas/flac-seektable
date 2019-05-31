#include "FLAC/all.h"

#include <nan.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <queue>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>

namespace flacseektable {

using node::AtExit;

using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::String;
using v8::Value;
using v8::Array;
using v8::Number;

using Nan::GetFunction;
using Nan::New;
using Nan::Set;

typedef std::queue< std::string > BufferQueue;
typedef std::vector< std::string > Tags;

struct ClientData {
	BufferQueue buffers;
	Tags tags;
	std::string vendor;

	const char* data;
	size_t length;
	size_t position;
	size_t absolutePosition;
	bool end;

	FLAC__StreamMetadata* seekBlock;
	FLAC__uint64 audio_offset;

	// StreamInfo
	unsigned min_blocksize;
	unsigned max_blocksize;
	unsigned min_framesize;
	unsigned max_framesize;
	unsigned sample_rate;
	unsigned channels;
	unsigned bits_per_sample;
	FLAC__uint64 total_samples;
	FLAC__byte md5sum[16];

	// Seektable construction
	FLAC__uint64 samples_written;
	FLAC__uint64 last_offset;
	unsigned first_seekpoint_to_check;
};

void zero_clientdata(ClientData* cd) {
	assert(cd);

	// Internal
	cd->data = nullptr;
	cd->length = 0;
	cd->position = 0;
	cd->absolutePosition = 0;
	cd->end = 0;
	cd->seekBlock = nullptr;
	cd->audio_offset = 0;

	// StreamInfo
	cd->min_blocksize = 0;
	cd->max_blocksize = 0;
	cd->min_framesize = 0;
	cd->max_framesize = 0;
	cd->sample_rate = 0;
	cd->channels = 0;
	cd->bits_per_sample = 0;
	cd->total_samples = 0;
	memset(cd->md5sum, 0, sizeof(FLAC__byte) * 16);

	// Seektable construction
	cd->samples_written = 0;
	cd->last_offset = 0;
	cd->first_seekpoint_to_check = 0;
}

FLAC__StreamDecoder* decoder = nullptr;
ClientData* clientData = nullptr;

void error_callback(const FLAC__StreamDecoder* decoder, FLAC__StreamDecoderErrorStatus status, void* client_data) {
	// TODO: error callback to JS side

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
	if (cd->total_samples == 0)
		return true;
	assert(cd->sample_rate > 0);
	unsigned samples = (unsigned)(1.0 * (double)cd->sample_rate);
	/* Restrict seekpoints to two per second of audio. */
	samples = samples < cd->sample_rate / 2 ? cd->sample_rate / 2 : samples;
	if (samples > 0) {
		/* +1 for the initial point at sample 0 */
		if (!FLAC__metadata_object_seektable_template_append_spaced_points_by_samples(cd->seekBlock, samples, cd->total_samples))
			return false;
	}
	return true;
}

std::string vorbis_to_string(const FLAC__StreamMetadata_VorbisComment_Entry& entry) {
	return std::string((const char*)entry.entry, entry.length);
}

void metadata_callback(const FLAC__StreamDecoder* decoder, const FLAC__StreamMetadata* metadata, void* client_data) {
	ClientData* cd = (ClientData*)client_data;
	switch (metadata->type) {
		case FLAC__METADATA_TYPE_STREAMINFO:
			cd->min_blocksize = metadata->data.stream_info.min_blocksize;
			cd->max_blocksize = metadata->data.stream_info.max_blocksize;
			cd->min_framesize = metadata->data.stream_info.min_framesize;
			cd->max_framesize = metadata->data.stream_info.max_framesize;
			cd->sample_rate = metadata->data.stream_info.sample_rate;
			cd->channels = metadata->data.stream_info.channels;
			cd->bits_per_sample = metadata->data.stream_info.bits_per_sample;
			cd->total_samples = metadata->data.stream_info.total_samples;
			memcpy(cd->md5sum, metadata->data.stream_info.md5sum, sizeof(FLAC__byte) * 16);
			break;

		case FLAC__METADATA_TYPE_VORBIS_COMMENT: {
			cd->vendor = vorbis_to_string(metadata->data.vorbis_comment.vendor_string);
			const FLAC__StreamMetadata_VorbisComment_Entry* comments = metadata->data.vorbis_comment.comments;
			for (unsigned i=0; i<metadata->data.vorbis_comment.num_comments; ++i) {
				cd->tags.push_back(vorbis_to_string(comments[i]));
			}
		}
		break;

		case FLAC__METADATA_TYPE_PADDING:
		case FLAC__METADATA_TYPE_APPLICATION:
		case FLAC__METADATA_TYPE_SEEKTABLE:
		case FLAC__METADATA_TYPE_CUESHEET:
		case FLAC__METADATA_TYPE_PICTURE:
		case FLAC__METADATA_TYPE_UNDEFINED:
		default:
			break;
	}

	if (metadata->is_last) {
		FLAC__stream_decoder_get_decode_position(decoder, &cd->audio_offset);
		cd->last_offset = cd->audio_offset;
		cd->seekBlock = FLAC__metadata_object_new(FLAC__METADATA_TYPE_SEEKTABLE);
		if (!gen_template(decoder, cd)) {
			// TODO: JS error callback
			std::cout << "ERROR: gen_template failed" << std::endl;
		}
	}
}

/** The method of determining the actual seek point values is mostly taken from the metaflac tool. */
FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder* decoder, const FLAC__Frame* frame, const FLAC__int32 * const buffer[], void *client_data) {
	ClientData* cd = (ClientData*)client_data;

	(void)buffer;
	assert(cd);

	FLAC__StreamMetadata_SeekTable* seek_table = &cd->seekBlock->data.seek_table;
	assert(seek_table);

	const unsigned blocksize = frame->header.blocksize;
	const FLAC__uint64 frame_first_sample = cd->samples_written;
	const FLAC__uint64 frame_last_sample = frame_first_sample + (FLAC__uint64)blocksize - 1;
	FLAC__uint64 test_sample;
	unsigned i;
	for (i = cd->first_seekpoint_to_check; i < seek_table->num_points; i++) {
		test_sample = seek_table->points[i].sample_number;
		if (test_sample > frame_last_sample) {
			break;
		}
		else if (test_sample >= frame_first_sample) {
			seek_table->points[i].sample_number = frame_first_sample;
			seek_table->points[i].stream_offset = cd->last_offset - cd->audio_offset;
			seek_table->points[i].frame_samples = blocksize;
			cd->first_seekpoint_to_check++;
			/* DO NOT: "break;" and here's why:
			 * The seektable template may contain more than one target
			 * sample for any given frame; we will keep looping, generating
			 * duplicate seekpoints for them, and we'll clean it up later,
			 * just before writing the seektable back to the metadata.
			 */
		}
		else {
			cd->first_seekpoint_to_check++;
		}
	}
	cd->samples_written += blocksize;
	if(!FLAC__stream_decoder_get_decode_position(decoder, &cd->last_offset))
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
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

	// Make sure we have data to operate with
	if (cd->buffers.size() == 0 && cd->end) {
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}

	if (!cd->data) {
		std::string& buf = cd->buffers.front();
		cd->position = 0;
		cd->data = buf.data();
		cd->length = buf.length();
	}

	size_t chunkLength = cd->length - cd->position;
	chunkLength = chunkLength > *bytes ? *bytes : chunkLength;

	memcpy(buffer, cd->data + cd->position, chunkLength);
	cd->position += chunkLength;
	cd->absolutePosition += chunkLength;
	*bytes = chunkLength;

	// When at the end of the first buffer, switch to the next one
	if (cd->position == cd->length) {
		cd->position = 0;
		cd->length = 0;
		cd->data = nullptr;
		cd->buffers.pop();
	}

	// End the stream if we're out of data
	if (cd->buffers.size() == 0 && cd->end) {
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

void process_buffers() {
	while (1) {
		FLAC__bool status = FLAC__stream_decoder_process_single(decoder);
		if (!status) {
			break;
		}

		// Return to allow more data to be passed from the JS side
		if (!clientData->end && clientData->buffers.size() < 2) {
			break;
		}

		if (clientData->end)
			break;
	}
}

NAN_METHOD(process_packet) {
	if (info.Length() != 1) {
		Nan::ThrowError("process_packet: requires 1 argument: buffer");
		return;
	}

	Local<Object> bufferObj = info[0]->ToObject(v8::Isolate::GetCurrent());
	size_t bufferLength = node::Buffer::Length(bufferObj);
	char* bufferData = node::Buffer::Data(bufferObj);
	if (bufferLength == 0 || !bufferData) {
		return;
	}

	clientData->buffers.push(std::string(bufferData, bufferLength));
	process_buffers();
}

NAN_METHOD(init) {
	decoder = FLAC__stream_decoder_new();
	FLAC__stream_decoder_set_metadata_respond_all(decoder);

	clientData = new ClientData();
	zero_clientdata(clientData);

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
	// let the decoder know this is it
	clientData->end = true;
	process_buffers();

	FLAC__StreamMetadata_SeekTable* seek_table = &clientData->seekBlock->data.seek_table;
	FLAC__format_seektable_sort(seek_table);
}

NAN_METHOD(get_data) {
	if (!clientData->end) {
		info.GetReturnValue().Set(false);
		return;
	}

	Local<Object> ret = New<Object>();
	Local<Array> seekpoints = New<Array>();

	Set(ret, New("seekpoints").ToLocalChecked(), seekpoints);
	Set(ret, New("audio_offset").ToLocalChecked(), New<Number>(clientData->audio_offset));

	Set(ret, New("min_blocksize").ToLocalChecked(), New<Number>(clientData->min_blocksize));
	Set(ret, New("max_blocksize").ToLocalChecked(), New<Number>(clientData->max_blocksize));
	Set(ret, New("min_framesize").ToLocalChecked(), New<Number>(clientData->min_framesize));
	Set(ret, New("max_framesize").ToLocalChecked(), New<Number>(clientData->max_framesize));
	Set(ret, New("sample_rate").ToLocalChecked(), New<Number>(clientData->sample_rate));
	Set(ret, New("channels").ToLocalChecked(), New<Number>(clientData->channels));
	Set(ret, New("bits_per_sample").ToLocalChecked(), New<Number>(clientData->bits_per_sample));
	Set(ret, New("total_samples").ToLocalChecked(), New<Number>(clientData->total_samples));

	Set(ret, New("vendor").ToLocalChecked(), New(clientData->vendor).ToLocalChecked());

	{
		std::stringstream stream;
		stream << std::hex;
		for (uint8_t i=0; i<16; ++i) {
			stream << (int)clientData->md5sum[i];
		}
		Set(ret, New("md5sum").ToLocalChecked(), New(stream.str()).ToLocalChecked());
	}

	{
		Local<Array> tags = New<Array>();
		unsigned index = 0;
		for (Tags::const_iterator it = clientData->tags.begin(); it != clientData->tags.end(); ++it) {
			tags->Set(index, New(*it).ToLocalChecked());
			++index;
		}
		Set(ret, New("tags").ToLocalChecked(), tags);
	}

	FLAC__StreamMetadata_SeekTable* seek_table = &clientData->seekBlock->data.seek_table;
	for (unsigned i=0; i<seek_table->num_points; ++i) {
		Local<Object> pt = New<Object>();
		Set(pt, New("sample").ToLocalChecked(), New<Number>(seek_table->points[i].sample_number));
		Set(pt, New("offset").ToLocalChecked(), New<Number>(seek_table->points[i].stream_offset));
		seekpoints->Set(i, pt);
	}

	info.GetReturnValue().Set(ret);
}

NAN_METHOD(clear) {
	// clean up
	FLAC__metadata_object_delete(clientData->seekBlock);
	FLAC__stream_decoder_delete(decoder);
	decoder = nullptr;
	delete clientData;
	clientData = nullptr;
}

void shutdown(void*) {
	if (clientData) {
		FLAC__metadata_object_delete(clientData->seekBlock);
		delete clientData;
		clientData = nullptr;
	}
	if (decoder) {
		FLAC__stream_decoder_delete(decoder);
		decoder = nullptr;
	}
}

NAN_MODULE_INIT(initialize) {
	AtExit(shutdown);

	NAN_EXPORT(target, init);
	NAN_EXPORT(target, process_packet);
	NAN_EXPORT(target, end);
	NAN_EXPORT(target, get_data);
	NAN_EXPORT(target, clear);
}

NODE_MODULE(flacseektable, initialize)

}
