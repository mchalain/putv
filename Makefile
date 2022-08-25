include scripts.mk

package=putv
version=2.1

subdir-$(JSONRPC)+=lib/jsonrpc
subdir-y+=src
subdir-$(TESTS)+=tests
subdir-y+=clients
subdir-$(WEBAPP)+=www
