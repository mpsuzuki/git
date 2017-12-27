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

	gname=`./parse-tar-file.py --print=gname --fail-if-multi $1`
	if test $? != 0 -o x"${gname}" != "x"$5
	then
	  echo "(some) gname differs from the specified value"
	  return $?
        fi

	return 0
}

git init . 1>/dev/null 2>/dev/null
touch uid-gid-test.001
mkdir uid-gid-test.002
mkdir uid-gid-test.002/uid-gid-test.003
git add uid-gid-test.001
git add uid-gid-test.002
git add uid-gid-test.002/uid-gid-test.003
git commit -m "uid-gid-test" 2>/dev/null 1>/dev/null

test_expect_success 'test a case with explicitly specified name/id, owner=nobody:1234 group=nogroup:5678' '
	git archive --format=tar --owner nobody:1234 --group nogroup:5678 HEAD > uid-gid-test1.tar &&
	check_uid_gid_uname_gname_in_tar uid-gid-test1.tar 1234 5678 nobody nogroup &&
	test_result=$? &&
	return ${test_result}
'

test_expect_success 'test a case with only name is specified, owner=(current my name) group=(current my group)' '
	my_uid=`id -u` &&
	my_gid=`id -g` &&
	my_uname=`id -u -n` &&
	my_gname=`id -g -n` &&
	git archive --format=tar --owner ${my_uname} --group ${my_gname} HEAD > uid-gid-test2.tar &&
	check_uid_gid_uname_gname_in_tar uid-gid-test2.tar ${my_uid} ${my_gid} ${my_uname} ${my_gname} &&
	test_result=$? &&
	return ${test_result}
'

test_done
