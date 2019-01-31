create table media ("id" INTEGER PRIMARY KEY, "url" TEXT UNIQUE NOT NULL, "mime" TEXT, "info" BLOB, "opusid" INTEGER, FOREIGN KEY (opusid) REFERENCES opus(id) ON UPDATE SET NULL);
create table opus ("id" INTEGER PRIMARY KEY,  "titleid" INTEGER UNIQUE NOT NULL, "artistid" INTEGER, "otherid" INTEGER, "albumid" INTEGER, "genreid" INTEGER, FOREIGN KEY (titleid) REFERENCES word(id), FOREIGN KEY (artistid) REFERENCES artist(id) ON UPDATE SET NULL, FOREIGN KEY (albumid) REFERENCES album(id) ON UPDATE SET NULL, FOREIGN KEY (genreid) REFERENCES word(id) ON UPDATE SET NULL);
create table album ("id" INTEGER PRIMARY KEY, "wordid" INTEGER UNIQUE NOT NULL, "artistid" INTEGER, "genreid" INTEGER, FOREIGN KEY (wordid) REFERENCES word(id), FOREIGN KEY (artistid) REFERENCES artist(id) ON UPDATE SET NULL, FOREIGN KEY (genreid) REFERENCES word(id) ON UPDATE SET NULL);
create table artist ("id" INTEGER PRIMARY KEY, "wordid" INTEGER UNIQUE NOT NULL, "info" BLOB, FOREIGN KEY (wordid) REFERENCES word(id));
create table genre ("id" INTEGER PRIMARY KEY, "wordid" INTEGER, FOREIGN KEY (wordid) REFERENCES word(id));
create table playlist ("id" INTEGER, FOREIGN KEY (id) REFERENCES opus(id) ON UPDATE SET NULL);
create table word ("id" INTEGER PRIMARY KEY, "name" TEXT UNIQUE NOT NULL);
