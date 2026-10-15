Dexter LDIMG extractor program

2026 OM3GAZX.

May be redistributed freely, just make sure to credit me.

I don't care for what this project is used for. As long as it does good, that's okay with me.

Installing FFmpeg is required for usage.

Build:

  gcc -O2 -Wall -Wextra main.c -o ldimg-decode

Usage:

  ./ldimg-decode [--video] FILE

  ./ldimg-decode --jpeg-only FILE

  ./ldimg-decode --wav FILE

  ./ldimg-decode --flac FILE

  ./ldimg-decode --merge FILE

  ./ldimg-decode --vbi FILE

  ./ldimg-decode --vbi-mame FILE

The default argument is --video, if no argument is provided.

Before extracting the LDIMG's contents, the file's metadata is printed for verification.

ldimg-decode is designed to be lightning fast, so the file's table of contents is read directly, instead of playing back the entire video.

Before extracting frames, the program waits for your input to confirm the metadata is correct, and to check if you have enough space on disk for the raw AVI extraction.

Pressing "Y" will begin frame extraction and video encoding, "N" will simply terminate the program.

If you only want the frame files to be extracted, use the --jpeg-only argument.

ldimg-decode extracts frames in bulks, which are 1000 per bulk by default. (Operating systems have a hard time indexing all files at once.)

Use --batch-size to change this, at your own risk.

ldimg-decode can extract audio in two formats:

  --wav:  The raw, unmodified audio embedded in the LDIMG, at 44.1 kHz, with 16-bit PCM.

  --flac: Re-encodes the embedded audio to a lossless format.

Finally, ldimg-decode includes options for merging the audio and video tracks and extracting the VBI metadata in MAME or Dexter-compatible formats:

--merge:    Merges the extracted video and audio files.

--vbi:      Extracts the VBI metadata from the LDIMG in Dexter format, completely unmodified.

--vbi-mame: Extracts the VBI metadata, and converts it to MAME's format.

As a warning, AVI outputs can get to be as large as 47GBs. Make sure you have enough
disk space to store the extracted video, and to perform subsequent operations (such
as compressing the merged AVI into a CHD).
