#! /bin/bash
# Ignore IO error if either of condtions are true
#   - '/var/db' is mounted as read-only.
#   - shutdown/reboot is ongoing.
# Match 'ro' only as a whole mount option: a plain word match also hits the
# common ext4 option 'errors=remount-ro' on a read-write mount.
awk '$2 == "/var/db" { print $4 }' /proc/mounts | grep -qE '(^|,)ro(,|$)' && exit

# checks whether reboot/shutdown is ongoing
initctl status shutdown | grep -q running && { touch /tmp/shutdown_running; exit; }
initctl status reboot | grep -q running && exit

# check available disk space
BYTES_FREE=$(($(stat -f -c "%a*%s" /mnt/lg/cmn_data)))
BYTES_THRESHOLD=$((100*1024*1024))  # 100M will be enough for database backup

# if enough disk space, backup current database for feature analisys.
# Only remove the corrupted database once the backup actually succeeded;
# a failed tar (ENOSPC race, IO error) must not destroy the evidence.
if [ "$BYTES_FREE" -ge "$BYTES_THRESHOLD" ]; then
    mkdir -p /mnt/lg/cmn_data/db8
    BACKUP_FILENAME="/mnt/lg/cmn_data/db8/corrupted_maindb.tar.bz2"
    rm -f "$BACKUP_FILENAME"
    if tar cjf "$BACKUP_FILENAME" /var/db/main; then
        chmod 400 "$BACKUP_FILENAME"
    else
        rm -f "$BACKUP_FILENAME"
        PmLogCtl log DB8 crit "mojodb-luna [] DB8 DBGMSG {} [maindb_errorOpenmainDb.bash] Backup of corrupted maindb failed"
    fi
    rm -rf /var/db/main/*
    rm -f /var/luna/preferences/ran-firstuse || true
else
    mkdir -p /mnt/lg/cmn_data/db8 || true
    touch /mnt/lg/cmn_data/db8/errorOpenMainDb || true
    rm -rf /var/db/main/* || true
    rm -f /var/luna/preferences/ran-firstuse || true
    PmLogCtl log DB8 crit "mojodb-luna [] DB8 DBGMSG {} [maindb_errorOpenmainDb.bash] No space left to store corrupted maindb"
fi

#do factory reset
/usr/bin/luna-send -n 1 luna://com.webos.service.tv.systemproperty/doUserDefault '{}'
