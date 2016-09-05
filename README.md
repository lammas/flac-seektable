# flac-seektable

Node.js stream tap for generating a seek table for a FLAC file stream.
It also supports extracting some of the metadata in the FLAC header (StreamInfo and Vorbis Comments blocks).

## Install

```sh
# install dependencies
apt-get install libflac-dev

# install the node module
npm install flac-seektable
```

## Limitations

* Currently only generates the seek table at 1s intervals.
* Errors detected during FLAC decoding are not propagated to the JS side.
* The Vorbis comments data is forwarded as-is and the key-value pairs are not parsed.

All of the above limitations will be addressed in future releases.

## Usage

```javascript
const fs = require('fs');
const seektable = require('flac-seektable');

var reader = fs.createReadStream('input.flac');
var writer = fs.createWriteStream('output.flac');
reader
	.pipe(new seektable(function(data) {
		console.log(data);
		/* would output a structure similar to this (or false in case an error occurred):
		{
			seekpoints: [
				{ sample: 0, offset: 0 },
				{ sample: 12288, offset: 11347 },
				{ sample: 28672, offset: 25690 },
				... etc ...
			],
			audio_offset: 136,
			min_blocksize: 4096,
			max_blocksize: 4096,
			min_framesize: 555,
			max_framesize: 4300,
			sample_rate: 16000,
			channels: 1,
			bits_per_sample: 16,
			total_samples: 107200,
			vendor: 'reference libFLAC 1.3.0 20130526',
			md5sum: 'e95f39f3d8b28b55181283cfd7ac98e7',
			tags: [
				// Vorbis Comment format key-value pairs
				'Comment=Processed by SoX'
			]
		}
		*/
	}))
	.pipe(writer);
```

### The seekpoints

```javascript
{
	sample: 123, // The sample number
	offset: 321  // The offset from the start of the audio stream
}

// The absolute seek position in the FLAC file would then be:
data.audio_offset + data.seekpoints[n].offset
// Where n is an integer in the range [0, data.seekpoints.length)
```
