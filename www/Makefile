HTDOCS?=htdocs/
CGI_BIN?=cgi-bin/
WEBSOCKET?=websocket/
CERTDIR?=ssl/

data-y+=$(HTDOCS)apps/ouiradio.html
data-y+=$(HTDOCS)apps/ouiradio.json
data-y+=$(HTDOCS)apps/apps.json
data-y+=$(HTDOCS)apps/js/player.js
data-y+=$(CGI_BIN)apps/totem-start.cgi

data-$(DBMNG)+=$(HTDOCS)apps/musics.html
data-$(DBMNG)+=$(CGI_BIN)apps/checkdb.cgi

sysconf-y+=ouistiti.d/putv.conf
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
hook-install-y+=webcommon_link
webcommon_link:
	ln -sf /srv/www-common/htdocs/common $(DESTDIR)$(datadir)/$(HTDOCS)
	ln -sf /srv/www-common/htdocs/trust $(DESTDIR)$(datadir)/$(HTDOCS)
	ln -sf /srv/www-common/htdocs/index.html $(DESTDIR)$(datadir)/$(HTDOCS)
