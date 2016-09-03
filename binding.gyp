{
	"targets": [
		{
			"target_name": "flac-seektable",
			"sources": [
				"src/main.cpp"
			],
			"include_dirs": [
				"<!(node -e \"require('nan')\")"
			],
			"libraries": [
				"-lFLAC",
			]
		}
	]
}
