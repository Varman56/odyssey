#!/bin/bash -x

set -ex

#
# TCP
#

/usr/bin/odyssey /tests/hba/tcp.conf

PGPASSWORD=correct_password psql -h ip4-localhost -p 6432 -U user_allow -c "SELECT 1" hba_db > /dev/null 2>&1 || {
  echo "ERROR: failed auth with hba trust, correct password and plain password in config"

	cat /var/log/odyssey.log
	echo "

	"
	cat /var/log/postgresql/postgresql-16-main.log

	exit 1
}

PGPASSWORD=incorrect_password psql -h ip4-localhost -p 6432 -U user_allow -c "SELECT 1" hba_db > /dev/null 2>&1 && {
  echo "ERROR: successfully auth with hba trust, but incorrect password"

	cat /var/log/odyssey.log
	echo "

	"
	cat /var/log/postgresql/postgresql-16-main.log

	exit 1
}

PGPASSWORD=correct_password psql -h ip4-localhost -p 6432 -U user_reject -c "SELECT 1" hba_db > /dev/null 2>&1 && {
  echo "ERROR: successfully auth with hba reject"

	cat /var/log/odyssey.log
	echo "

	"
	cat /var/log/postgresql/postgresql-16-main.log

	exit 1
}

PGPASSWORD=correct_password psql -h ip4-localhost -p 6432 -U user_unknown -c "SELECT 1" hba_db > /dev/null 2>&1 && {
  echo "ERROR: successfully auth without hba rule"

	cat /var/log/odyssey.log
	echo "

	"
	cat /var/log/postgresql/postgresql-16-main.log

	exit 1
}  

kill -s HUP $(pgrep odyssey)
PGPASSWORD=correct_password PGCONNECT_TIMEOUT=5 psql -h ip4-localhost -p 6432 -U user_allow -c "SELECT 1" hba_db > /dev/null 2>&1 || {
  echo "ERROR: unable to connect after SIGHUP"

	cat /var/log/odyssey.log
	echo "
	"
	cat /var/log/postgresql/postgresql-16-main.log

	exit 1
}

ody-stop

#
# Unix
#

/usr/bin/odyssey /tests/hba/unix.conf

PGPASSWORD=correct_password psql -h /tmp -p 6432 -U user_allow -c "SELECT 1" hba_db > /dev/null 2>&1 || {
    echo "ERROR: failed auth with hba trust, correct password and plain password in config"

	cat /var/log/odyssey.log
	echo "

	"
	cat /var/log/postgresql/postgresql-16-main.log

	exit 1
}

PGPASSWORD=correct_password psql -h /tmp -p 6432 -U user_reject -c "SELECT 1" hba_db > /dev/null 2>&1 && {
  echo "ERROR: successfully auth with hba reject"

	cat /var/log/odyssey.log
	echo "

	"
	cat /var/log/postgresql/postgresql-16-main.log

	exit 1
}

ody-stop
