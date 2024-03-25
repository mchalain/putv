include scripts.mk

package=putv
version=2.1

subdir-$(TINYSVCMDNS_INTERN)+=lib/tinysvcmdns.mk
subdir-$(JSONRPC)+=lib/jsonrpc
subdir-y+=src
subdir-$(TESTS)+=tests
subdir-y+=clients
subdir-$(WEBAPP)+=www
