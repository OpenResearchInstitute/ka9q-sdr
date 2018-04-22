// $Id: attr.h,v 1.2 2018/04/22 18:21:01 karn Exp $
// Routines for reading and writing formatted text strings to external file attributes
// Should be portable to Linux and Mac OSX, which are gratuitously different
// Copyright 2018, Phil Karn, KA9Q

int attrscanf(int fd,char const *name,char const *format, ...);
int attrprintf(int fd,char const *attr,char const *format, ...);
