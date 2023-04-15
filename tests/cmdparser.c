#include <stdio.h>

int main(int argc, char * argv[])
{
	char buf[256];
	setvbuf(stdin, buf, _IOLBF, 256);
	do
	{
		char c = fgetc(stdin);
		if (c == 'p')
			printf("\n%s\n", buf);
		if (c == 'q')
			break;
	} while(1);
	return 0;
}
