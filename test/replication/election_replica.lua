#!/usr/bin/env tarantool

local INSTANCE_ID = string.match(arg[0], "%d")
local SOCKET_DIR = require('fio').cwd()
local SYNCHRO_QUORUM = arg[1] and tonumber(arg[1]) or 3

local function instance_uri(instance_id)
    return SOCKET_DIR..'/election_replica'..instance_id..'.sock';
end

require('console').listen(os.getenv('ADMIN'))

box.cfg({
    listen = instance_uri(INSTANCE_ID),
    replication = {
        instance_uri(1),
        instance_uri(2),
        instance_uri(3),
    },
    replication_timeout = 0.1,
    election_is_enabled = true,
    election_is_candidate = true,
    -- Should be at least as big as replication_disconnect_timeout, which is
    -- 4 * replication_timeout.
    election_timeout = 0.4,
    replication_synchro_quorum = SYNCHRO_QUORUM,
    replication_synchro_timeout = 0.1,
    -- To reveal more election logs.
    log_level = 6,
})

box.once("bootstrap", function()
    box.schema.user.grant('guest', 'super')
end)
