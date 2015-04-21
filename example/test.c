#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <nikolav2g.h>
#include <polarssl/x509.h>
#include <polarssl/error.h>
#include "slac/plc_eth.h"
#include "client.h"
#include "server.h"


int slac_associate(const char *if_name);
void plgp_slac_listen(const char *if_name, const uint8_t dest_mac_evse[6]);

static const uint8_t EVMAC[6] = {0x00, 0x05, 0xB6, 0x01, 0x86, 0xBD};
static const uint8_t EVSEMAC[6] = {0x00, 0x05, 0xB6, 0x01, 0x88, 0xA3};

static const char *argv0;

static void fatal(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: ", argv0);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");

    exit(1);
}

static void ev(const char *if_name, bool tls_enabled)
{
    evcc_conn_t conn;
    ev_session_t s;
    memset(&conn, 0, sizeof(evcc_conn_t));
    memset(&s, 0, sizeof(s));
    int err;
    if (load_contract("certs/contractchain.pem", "certs/contract.key", &s) != 0) {
        fatal("can't load certs/contract.key: %m");
    }
    if (ev_sdp_discover_evse(if_name, &conn.addr, tls_enabled) < 0) {
        fatal("failed to discover EVSE on interface %s", if_name);
    }
    printf("connecting to secc\n");
    if (tls_enabled) {
        err = evcc_connect_tls(&conn, "certs/ev.pem", "certs/ev.key");
    } else {
        err = evcc_connect_tcp(&conn);
    }
    if (err != 0) {
        printf("main: evcc_connect_tls error\n");
        return;
    }
    printf("session setup request\n");
    err = session_request(&conn, &s);
    if (err != 0) {
        printf("RIP session_request\n");
        return;
    }
    printf("service discovery request\n");
    err = service_discovery_request(&conn, &s);
    if (err != 0) {
        printf("ev_example: service discovery request err\n");
        return;
    }
    printf("payment selection request\n");
    err = payment_selection_request(&conn, &s);
    if (err != 0) {
        printf("ev_example: payment_selection_request err\n");
        return;
    }
    printf("payment details request\n");
    if (!s.charging_is_free) {
        err = payment_details_request(&conn, &s);
        if (err != 0) {
            printf("ev_example: payment_selection_request err\n");
            return;
        }
    }
    printf("authorization request\n");
    err = authorization_request(&conn, &s);
    if (err != 0) {
        printf("ev_example: authorization_request err\n");
        return;
    }
    printf("charge parameter request\n");
    err = charge_parameter_request(&conn, &s);
    if (err != 0) {
        printf("ev_example: charge_parameter_request err\n");
        return;
    }
    printf("power delivery request\n");
    err = power_delivery_request(&conn, &s);
    if (err != 0) {
        printf("ev_example: power_delivery_request err\n");
        return;
    }
    printf("Charging (repeating charging status requests)\n");
    for (int i = 0;i < 2; i++) {
        err = charging_status_request(&conn, &s);
        if (err != 0) {
            printf("ev_example: charging_status_request err\n");
            return;
        }
        printf("=");
        fflush(stdout);
        sleep(1);
    }
    printf("Performing session stop request\n");
    err = session_stop_request(&conn, &s);
    if (err != 0) {
        printf("ev_example: session_stop_request err\n");
        return;
    }
    evcc_close_conn(&conn);
    evcc_session_cleanup(&s);
    printf("Finished charging, ending session\n");
}

static void evse(const char *if_name)
{
    int tls_port, tcp_port, tls_sockfd, tcp_sockfd;

    // Init the contract root certificates
    int err = x509_crt_parse_path(&Trusted_contract_rootcert_chain,
                                  "certs/root/mobilityop/certs/");
    if (err != 0) {
        printf("evse_example: Unable to load contract root certificates\n");
        char strerr[256];
        polarssl_strerror(err, strerr, 256);
        printf("err = %s\n", strerr);
        return;
    }
    init_sessions();
    // === Bind to dynamic port ===
    tls_sockfd = bind_v2gport(&tls_port);
    if (tls_sockfd < 0) {
        printf("secc_bind_tls  returned %d\n", tls_sockfd);
        return;
    }
    tcp_sockfd = bind_v2gport(&tcp_port);
    if (tcp_sockfd < 0) {
        printf("secc_bind_tls  returned %d\n", tcp_sockfd);
        return;
    }
    printf("start sdp listen\n");
    secc_listen_tls(tls_sockfd, &create_response_message, "certs/evse.pem", "certs/evse.key");
    secc_listen_tcp(tcp_sockfd, &create_response_message);
    // Set port to 0 to disable tls or tcp
    // (always do sdp_listen after secc_listen_*)
    sdp_listen(if_name, tls_port, tcp_port);
}

void usage(void)
{
    fprintf(stderr, "Usage: %s [-sv] [--] interface node-type\n", argv0);
    exit(1);
}

void
threadmain(int argc,
       char *argv[])
{
    enum { EV, EVSE };
    const char *iface, *type;
    int opt, slac = 0, notls = 0;

    argv0 = argv[0];
    while ((opt = getopt(argc, argv, "svnf")) != -1) {
        switch (opt) {
        case 's': // Enable SLAC
           slac++;
           break;

        case 'v': // Verbose
            chattyv2g++;
            break;
        case 'n': // no tls
            notls++;
            break;
        case 'f':
            secc_free_charge++;
            break;
        default:
            usage();
        }
    }
    if (optind + 1 >= argc) { usage(); }

    iface = argv[optind];
    type = argv[optind + 1];
    if (strcasecmp(type, "EVSE") == 0) {
        switch_power_line(iface, EVSEMAC, false);
        if (slac) {
            printf("SLAC enabled\n");
            plgp_slac_listen(iface, EVSEMAC);
        }
        evse(iface);
    } else if (strcasecmp(type, "EV") == 0) {
        if (slac) {
            switch_power_line(iface, EVMAC, false);
            printf("=== STARTING SLAC ASSOCIATION ===\n");
            while(slac_associate(iface) != 0) {
                printf("something went wrong, trying again\n");
            }
            printf("Slac is done. Waiting 8 seconds for networks to form.\n");
            sleep(8);
        }
       ev(iface, !notls);
    } else {
        fatal("node type must be EV or EVSE");
     }
    printf("Exiting\n");
    exit(0);
}
