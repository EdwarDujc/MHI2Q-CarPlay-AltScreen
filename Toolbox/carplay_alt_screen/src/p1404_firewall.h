/* p1404_firewall.h - runtime-scoped CarPlay type-111 PF aperture. */
#ifndef P1404_FIREWALL_H
#define P1404_FIREWALL_H
#include <stddef.h>
#include <stdint.h>
/* Value-only cleanup ownership: no receiver/ScreenSession pointer survives. */
struct p1404_pf_lease { uint16_t port; uint32_t generation; };
/* Installer-owned pf.conf aperture. The stock DHCP path reloads pf.conf and
 * drops every runtime rule; a rule in the file survives those reloads. */
#define P1404_ALT111_STATIC_PORT_FIRST 7200u
#define P1404_ALT111_STATIC_PORT_COUNT 4u
#define P1404_ALT111_STATIC_RULE \
    "pass in quick on carplay0 proto tcp from any to any port 7200:7203 allow-opts"
void p1404_alt111_firewall_maintenance_start(void);
int p1404_alt111_firewall_open(uint16_t port);
int p1404_alt111_firewall_close(uint16_t port);
int p1404_alt111_firewall_open_owned(uint16_t port, struct p1404_pf_lease *lease);
/* As open_owned; *ownership_conflict=1 when another owner holds this port. */
int p1404_alt111_firewall_open_owned_ex(uint16_t port, struct p1404_pf_lease *lease,
                                        int *ownership_conflict);
int p1404_alt111_firewall_close_owned(const struct p1404_pf_lease *lease);
int p1404_alt111_firewall_rewrite(const char *rules, uint16_t port, int enable,
                                  char *out, size_t out_cap, int *changed_out);
/* 1 when the static rule is effective (before the carplay0 block) for port. */
int p1404_alt111_firewall_static_covers(const char *rules, uint16_t port);
#endif
