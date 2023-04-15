#include <curl/curl.h>

int main(int argc, char *argv[])
{
	CURL *curl = curl_easy_init();
	if(curl && argc > 2) {
		FILE *output = fopen(argv[2], "w");
		CURLcode res;
		curl_easy_setopt(curl, CURLOPT_URL, argv[1]);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, output);
		res = curl_easy_perform(curl);
		curl_easy_cleanup(curl);
		fclose(output);
	}
	return 0;
}
