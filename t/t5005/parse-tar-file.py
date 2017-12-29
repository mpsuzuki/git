#!/usr/bin/env python

import sys
import getopt
import tarfile

optlist, args = getopt.getopt( sys.argv[1:], "", [
				"print=", "show=",
				"uniq",
				"fail-if-multi",
		] )

infos_to_print = []
uniq = False
fail_if_multi = False

for opt in optlist:
  if opt[0] == "--print":
    infos_to_print.append(opt[1])
  elif opt[0] == "--show":
    infos_to_print.append(opt[1])
  elif opt[0] == "--uniq":
    uniq = True
  elif opt[0] == "--fail-if-multi":
    uniq = True
    fail_if_multi = True

if len(infos_to_print) == 0:
  infos_to_print = ["uid", "gid", "uname", "gname", "name"]

if len(args) > 0:
  tar = tarfile.open( path=args[0], mode="r|" )
else:
  tar = tarfile.open( mode="r|", fileobj=sys.stdin )

out_lines = []
for tarinfo in tar:
  infos = []
  for info_tag in infos_to_print:
    if info_tag == "uid":
      infos.append( str(tarinfo.uid) )
    elif info_tag == "gid":
      infos.append( str(tarinfo.gid) )
    elif info_tag == "uname" or info_tag == "owner":
      infos.append( tarinfo.uname )
    elif info_tag == "gname" or info_tag == "group":
      infos.append( tarinfo.gname )
    elif info_tag == "name" or info_tag == "pathname":
      infos.append( tarinfo.name )
  out_lines.append( "\t".join(infos) )
tar.close()

if uniq:
  out_lines = list(set(out_lines))
  if fail_if_multi and (len(out_lines) > 1):
    sys.stderr.write("*** not unique value, " + str(len(out_lines)) + " values found\n")
    sys.exit(len(out_lines))

for line in out_lines:
  print line
