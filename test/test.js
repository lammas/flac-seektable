'use strict';

const fs = require('fs');
const test = require('tape');
const seektable = require('../index');

test('minimal', function (t) {
	var reader = fs.createReadStream('test/nyanya.flac');
	var writer = fs.createWriteStream('test/out.flac');
	console.time('timing');
	reader
		.pipe(new seektable(function(data) {
			t.equals(data.seekpoints.length, 7, 'Correct amount of seek points');
			t.equals(data.audio_offset, 136, 'Correct audio offset');
			console.timeEnd('timing');
			t.end();
		}))
		.pipe(writer);
});
