#!/bin/bash

# conda init
# conda activate central_controller

if [ -z "$ZT_DB_HOST" ]; then
    echo '*** FAILED: ZT_DB_HOST environment variable not defined'
    exit 1
fi
if [ -z "$ZT_DB_PORT" ]; then
    echo '*** FAILED: ZT_DB_PORT environment variable not defined'
    exit 1
fi
if [ -z "$ZT_DB_NAME" ]; then
    echo '*** FAILED: ZT_DB_NAME environment variable not defined'
    exit 1
fi
if [ -z "$ZT_DB_USER" ]; then
    echo '*** FAILED: ZT_DB_USER environment variable not defined'
    exit 1
fi
if [ -z "$ZT_DB_PASSWORD" ]; then
    echo '*** FAILED: ZT_DB_PASSWORD environment variable not defined'
    exit 1
fi

REDIS=""
if [ "$ZT_USE_REDIS" == "true" ]; then
    if [ -z "$ZT_REDIS_HOST" ]; then
        echo '*** FAILED: ZT_REDIS_HOST environment variable not defined'
        exit 1
    fi

    if [ -z "$ZT_REDIS_PORT" ]; then
        echo '*** FAILED: ZT_REDIS_PORT enivronment variable not defined'
        exit 1
    fi

    if [ -z "$ZT_REDIS_CLUSTER_MODE" ]; then
        echo '*** FAILED: ZT_REDIS_CLUSTER_MODE environment variable not defined'
        exit 1
    fi

    REDIS=", \"redis\": {
            \"hostname\": \"${ZT_REDIS_HOST}\",
            \"port\": ${ZT_REDIS_PORT},
            \"clusterMode\": ${ZT_REDIS_CLUSTER_MODE},
            \"password\": \"${ZT_REDIS_PASSWORD}\"
        }
    "
else
    REDIS=", \"redis\": null"
fi

mkdir -p /var/lib/zerotier-one

pushd /var/lib/zerotier-one
if [ -d "$ZT_IDENTITY_PATH" ]; then
    echo '*** Using existing ZT identity from path $ZT_IDENTITY_PATH'

    ln -s $ZT_IDENTITY_PATH/identity.public identity.public
    ln -s $ZT_IDENTITY_PATH/identity.secret identity.secret
    if [ -L  "$ZT_IDENTITY_PATH/authtoken.secret" ] && [ -e "$ZT_IDENTITY_PATH/authtoken.secret" ]; then
        ln -s $ZT_IDENTITY_PATH/authtoken.secret authtoken.secret
        ln -s $ZT_IDENTITY_PATH/authtoken.secret metricstoken.secret
    fi
fi
popd

DEFAULT_PORT=9993
DEFAULT_LB_MODE=false

APP_NAME="controller-$(cat /var/lib/zerotier-one/identity.public | cut -d ':' -f 1)"

BIGTABLE_CONF=""
if [ "$ZT_USE_BIGTABLE" == "true" ]; then
    if [ -z "$ZT_BIGTABLE_PROJECT" ] || [ -z "$ZT_BIGTABLE_INSTANCE" ] || [ -z "$ZT_BIGTABLE_TABLE" ]; then
        echo '*** FAILED: ZT_BIGTABLE_PROJECT, ZT_BIGTABLE_INSTANCE, and ZT_BIGTABLE_TABLE environment variables must all be defined to use Bigtable as a controller backend'
        exit 1
    fi

    BIGTABLE_CONF=", \"bigtable\": {
        \"project_id\": \"${ZT_BIGTABLE_PROJECT}\",
        \"instance_id\": \"${ZT_BIGTABLE_INSTANCE}\",
        \"table_id\": \"${ZT_BIGTABLE_TABLE}\"
    }
    "
fi


PUBSUB_CONF=""
if [ "$ZT_USE_PUBSUB" == "true" ]; then
    if [ -z "$ZT_PUBSUB_PROJECT" ]; then
        echo '*** FAILED: ZT_PUBSUB_PROJECT environment variable must be defined to use PubSub as a controller backend'
        exit 1
    fi

    if [ -z "$ZT_PUBSUB_MEMBER_CHANGE_RECV_TOPIC" ] || [ -z "$ZT_PUBSUB_MEMBER_CHANGE_SEND_TOPIC" ] || [ -z "$ZT_PUBSUB_NETWORK_CHANGE_RECV_TOPIC" ] || [ -z "$ZT_PUBSUB_NETWORK_CHANGE_SEND_TOPIC" ]; then
        echo '*** FAILED: ZT_PUBSUB_MEMBER_CHANGE_RECV_TOPIC, ZT_PUBSUB_MEMBER_CHANGE_SEND_TOPIC, ZT_PUBSUB_NETWORK_CHANGE_RECV_TOPIC, and ZT_PUBSUB_NETWORK_CHANGE_SEND_TOPIC environment variables must all be defined to use PubSub as a controller backend'
        exit 1
    fi

    PUBSUB_CONF=", \"pubsub\": {
        \"project_id\": \"${ZT_PUBSUB_PROJECT}\",
        \"member_change_recv_topic\": \"${ZT_PUBSUB_MEMBER_CHANGE_RECV_TOPIC}\",
        \"member_change_send_topic\": \"${ZT_PUBSUB_MEMBER_CHANGE_SEND_TOPIC}\",
        \"network_change_recv_topic\": \"${ZT_PUBSUB_NETWORK_CHANGE_RECV_TOPIC}\",
        \"network_change_send_topic\": \"${ZT_PUBSUB_NETWORK_CHANGE_SEND_TOPIC}\",
        \"sso_send_topic\": \"${ZT_PUBSUB_SSO_SEND_TOPIC}\",
        \"sso_recv_topic\": \"${ZT_PUBSUB_SSO_RECV_TOPIC}\"
    }
"
fi

echo "{
    \"settings\": {
        \"controllerDbPath\": \"postgres:host=${ZT_DB_HOST} port=${ZT_DB_PORT} dbname=${ZT_DB_NAME} user=${ZT_DB_USER} password=${ZT_DB_PASSWORD} application_name=${APP_NAME} sslmode=prefer sslcert=${DB_CLIENT_CERT} sslkey=${DB_CLIENT_KEY} sslrootcert=${DB_SERVER_CA}\",
        \"portMappingEnabled\": true,
        \"softwareUpdate\": \"disable\",
        \"interfacePrefixBlacklist\": [
            \"inot\",
            \"nat64\"
        ],
        \"lowBandwidthMode\": ${ZT_LB_MODE:-$DEFAULT_LB_MODE},
        \"ssoRedirectURL\": \"${ZT_SSO_REDIRECT_URL}\",
        \"allowManagementFrom\": [\"127.0.0.1\", \"::1\", \"10.0.0.0/8\"],
        \"otel\": {
            \"exporterEndpoint\": \"${ZT_EXPORTER_ENDPOINT}\",
            \"exporterSampleRate\": ${ZT_EXPORTER_SAMPLE_RATE:-0}
        }
        ${REDIS}
    },
    \"controller\": {
        \"listenMode\": \"${ZT_LISTEN_MODE:-pgsql}\",
        \"statusMode\": \"${ZT_STATUS_MODE:-pgsql}\"
        ${REDIS}
        ${BIGTABLE_CONF}
        ${PUBSUB_CONF}
    }
}    
" > /var/lib/zerotier-one/local.conf

if [ -n "$DB_SERVER_CA" ]; then
    echo "secret list"
    chmod 600 /secrets/db/*.pem
    ls -l /secrets/db/
    until pg_isready -h ${ZT_DB_HOST} -p ${ZT_DB_PORT} -d "sslmode=prefer sslcert=${DB_CLIENT_CERT} sslkey=${DB_CLIENT_KEY} sslrootcert=${DB_SERVER_CA}"; do
	    echo "Waiting for PostgreSQL...";
	    sleep 2;
    done
else
    until pg_isready -h ${ZT_DB_HOST} -p ${ZT_DB_PORT}; do
	    echo "Waiting for PostgreSQL...";
	    sleep 2;
    done
fi


echo "Migrating database (if needed)..."
if [ -n "$DB_SERVER_CA" ]; then
    /usr/local/bin/migrate -source file:///migrations -database "postgres://$ZT_DB_USER:$ZT_DB_PASSWORD@$ZT_DB_HOST:$ZT_DB_PORT/$ZT_DB_NAME?x-migrations-table=controller_migrations&sslmode=verify-full&sslrootcert=$DB_SERVER_CA&sslcert=$DB_CLIENT_CERT&sslkey=$DB_CLIENT_KEY" up  
else 
    /usr/local/bin/migrate -source file:///migrations -database "postgres://$ZT_DB_USER:$ZT_DB_PASSWORD@$ZT_DB_HOST:$ZT_DB_PORT/$ZT_DB_NAME?x-migrations-table=controller_migrations&sslmode=disable" up
fi

if [ -n "$ZT_TEMPORAL_HOST" ] && [ -n "$ZT_TEMPORAL_PORT" ]; then
    echo "waiting for temporal..."
    while ! nc -z ${ZT_TEMPORAL_HOST} ${ZT_TEMPORAL_PORT}; do
        echo "waiting...";
        sleep 1;
    done
    echo "Temporal is up"
fi

cat /var/lib/zerotier-one/local.conf

export GOOGLE_CLOUD_CPP_ENABLE_CLOG=yes
export LIBC_FATAL_STDERR_=1
export GLIBCXX_FORCE_NEW=1
export GLIBCPP_FORCE_NEW=1
export LD_PRELOAD="/opt/conda/envs/central_controller/lib/libjemalloc.so.2"
exec /usr/local/bin/zerotier-one -p${ZT_CONTROLLER_PORT:-$DEFAULT_PORT} /var/lib/zerotier-one
