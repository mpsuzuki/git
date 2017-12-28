#!/bin/sh

test_description='test --owner --group options for git-archive'
. ./test-lib.sh

check_uid_gid_uname_gname_in_tar() {
	# $1 tar pathname
	# $2 uid (digit in string)
	# $3 gid (digit in string)
	# $4 uname (string)
	# $5 gname (string)
	uid=`./parse-tar-file.py --print=uid --fail-if-multi $1`
	if test $? != 0 -o x"${uid}" != "x"$2
	then
	  echo "(some) uid differs from the specified value"
	  return $?
        fi

	gid=`./parse-tar-file.py --print=gid --fail-if-multi $1`
	if test $? != 0 -o x"${gid}" != "x"$3
	then
	  echo "(some) gid differs from the specified value"
	  return $?
	  exit $?
        fi

	uname=`./parse-tar-file.py --print=uname --fail-if-multi $1`
	if test $? != 0 -o x"${uname}" != "x"$4
	then
	  echo "(some) uname differs from the specified value"
	  return $?
        fi

	gname=`./parse-tar-file.py --print=uname --fail-if-multi $1`
	if test $? != 0 -o x"${gname}" != "x"$5
	then
	  echo "(some) gname differs from the specified value"
	  return $?
        fi

	return 0
}


test_expect_success 'test explicitly specified cases, owner=nobody:1234 group=nobody:5678' '
	git archive --format=tar --owner nobody:1234 --gid nogroup:5678 HEAD > uid-gid-test.tar
	check_uid_gid_uname_gname_in_tar(uid-gid-test.tar, "1234", "5678", "nobody", "nogroup")
	test_result=$?
	rm -f uid-gid-test.tar
	return ${test_result}
'

test_expect_success 'test only name specified cases, owner=(current my name) group=(current my group)' '
	my_uid=`id -u`
	my_gid=`id -g`
	my_uname=`id -u -n`
	my_gname=`id -g -n`
	git archive --format=tar --owner ${my_uname} --gid ${my_gname} HEAD > uid-gid-test.tar
	check_uid_gid_uname_gname_in_tar(uid-gid-test.tar, my_uid, my_gid, my_uname, my_gname)
	test_result=$?
	rm -f uid-gid-test.tar
	return ${test_result}
'
test_done
