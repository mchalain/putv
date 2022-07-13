#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

int main(void)
{
	int len = 1024;
	char inbuffer[1024];
	int input, output;
	
	input = open(argv[1], O_RDONLY);
	output = 1;
	
	len = read(inbuffer, buffer, len);
	int offset = NeAACDecInit(decoder, inbuffer,len, &samplerate, &channels);
	dbg("decoder faad: samplerate %lu fps, channels %d", samplerate, channels);
	memcpy(inbuffer, inbuffer + offset, len - offset);
	len -= offset;

	int ret = 0;
	do
	{

		NeAACDecFrameInfo frameInfo;
		void *samples = NeAACDecDecode(decoder, &frameInfo, inbuffer, len);
		if (frameInfo.error > 0)
		{
			err("decoder faad: error %s", NeAACDecGetErrorMessage(frameInfo.error));
			ret = -1;
			break;
		}
		write(output, samples, frameInfo.samples);
		offset += frameInfo.bytesconsumed;
		len -= frameInfo.bytesconsumed;
		if (len < 1024
		int tmp = read(input, inbuffer + offset, len
	} while(ret == 0);

	close(input);
	close(output);
