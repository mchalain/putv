HTDOCS?=htdocs/
CGI_BIN?=cgi-bin/apps/
WEBSOCKET?=websocket/
CERTDIR?=ssl/

data-y+=$(HTDOCS)apps/ouiradio.html
data-y+=$(HTDOCS)apps/ouiradio.json
data-y+=$(HTDOCS)apps/apps.json
data-y+=$(HTDOCS)apps/js/player.js
data-$(WEBCOMMON)+=$(HTDOCS)index.html
data-$(WEBCOMMON)+=$(HTDOCS)common/js/jsonrpc.js
data-$(WEBCOMMON)+=$(HTDOCS)common/js/cmdbar.js
data-$(WEBCOMMON)+=$(HTDOCS)common/js/auth.js
data-y+=$(CGI_BIN)totem-start.cgi

BOOTSTRAP_DOCS?=htdocs/trust
JQUERY_DOCS?=htdocs/trust
export BOOTSTRAP_DOCS JQUERY_DOCS
subdir-$(BOOTSTRAP)+=bootstrap-5.mk
subdir-$(JQUERY)+=jquery.mk

data-$(GLYPHICONS)+=$(HTDOCS)trust/css/glyphicons.css
data-$(GLYPHICONS)+=$(HTDOCS)trust/fonts/glyphicons-halflings-regular.woff2

sysconf-y+=ouistiti.d/putv.conf
sysconf-y+=passwd.db
sysconf-y+=filter.db
sysconf-y+=radio.db

hook-install-y+=websocket_dir
websocket_dir:
	mkdir -p $(DESTDIR)$(datadir)/$(WEBSOCKET)
hook-install-y+=conf_link
conf_link:
	rm -f $(DESTDIR)$(datadir)/conf
	ln -sf $(sysconfdir) $(DESTDIR)$(datadir)/conf
hook-install-y+=certs_link
certs_link:
	rm -f $(DESTDIR)$(datadir)/ssl
	ln -sf /etc/ssl/certs $(DESTDIR)$(datadir)/ssl
hook-install-y+=radio_link
radio_link:
	ln -sf $(sysconfdir)/radio.db $(DESTDIR)$(datadir)/$(HTDOCS)apps/
