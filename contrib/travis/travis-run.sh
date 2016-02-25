#!/bin/bash

set -ev

print_test_logs() {
    echo ">>>> PRINT TEST LOGS $1 (start)"
    find -regex '.*/tests/[^/]+.log$' -exec sh -c 'echo ">>>>> LOGFILE $1"; cat "$1"' '--' '{}' ';'
    echo ">>>> PRINT TEST LOGS $1 (done)"
}

sh ./autogen.sh \
    --with-systemd-logind=no \
    --enable-more-warnings=no

make -j6

if ! make check -k ; then

    print_test_logs "first-test"

    echo ">>>> RUN SECOND TEST (start)"
    NMTST_DEBUG=TRACE,no-expect-messages make check -k || :
    echo ">>>> RUN SECOND TEST (done)"

    print_test_logs "second-test"

    exit 57
fi

