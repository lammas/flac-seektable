'use strict';

const stream = require('stream');
const seektable = require('./build/Release/flac-seektable');

class SeekTable extends stream.Transform {
	constructor(callback, options) {
		super(options);
		seektable.init();

		this.on('end', function() {
			seektable.end();
			if (callback)
				callback(seektable.get_table());
			seektable.clear();
		});
	}

	_transform(chunk, encoding, done) {
		// console.log('JS:', chunk);
		seektable.process_packet(chunk);

		this.push(chunk);
		done();
	}
}

module.exports = SeekTable;
