#include <netinet/in.h>
#include <string.h>

#include "parse.h"

#define ETH_HDR_LEN    14
#define VLAN_TAG_LEN   4
#define MAX_VLAN_TAGS  2
#define IPV4_MIN_HDR   20
#define IPV6_HDR_LEN   40

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806
#define ETHERTYPE_VLAN 0x8100
#define ETHERTYPE_IPV6 0x86dd
#define ETHERTYPE_QINQ 0x88a8

static uint16_t
be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static enum parse_result
read_ports(const uint8_t *l4, size_t avail, struct pkt_info *info)
{
	if (avail < 4)
		return PARSE_MALFORMED;
	info->src_port = be16(l4);
	info->dst_port = be16(l4 + 2);
	return PARSE_IP;
}

static enum parse_result
parse_ipv4(const uint8_t *ip, size_t avail, struct pkt_info *info)
{
	size_t ihl, total;

	if (avail < IPV4_MIN_HDR || (ip[0] >> 4) != 4)
		return PARSE_MALFORMED;
	ihl = (size_t)(ip[0] & 0x0f) * 4;
	total = be16(ip + 2);
	// total may be below avail: Ethernet pads short frames.
	if (ihl < IPV4_MIN_HDR || ihl > avail || total < ihl || total > avail)
		return PARSE_MALFORMED;

	info->family = 4;
	info->proto = ip[9];
	memcpy(info->src_ip, ip + 12, 4);
	memcpy(info->dst_ip, ip + 16, 4);

	if ((be16(ip + 6) & 0x1fff) != 0)
		return PARSE_IP;
	if (info->proto == IPPROTO_TCP || info->proto == IPPROTO_UDP)
		return read_ports(ip + ihl, total - ihl, info);
	return PARSE_IP;
}

static enum parse_result
parse_ipv6(const uint8_t *ip, size_t avail, struct pkt_info *info)
{
	size_t payload, l4_avail;

	if (avail < IPV6_HDR_LEN || (ip[0] >> 4) != 6)
		return PARSE_MALFORMED;
	payload = be16(ip + 4);
	if (IPV6_HDR_LEN + payload > avail)
		return PARSE_MALFORMED;

	info->family = 6;
	info->proto = ip[6];
	memcpy(info->src_ip, ip + 8, 16);
	memcpy(info->dst_ip, ip + 24, 16);

	if (info->proto == IPPROTO_TCP || info->proto == IPPROTO_UDP) {
		// A payload length of 0 means a jumbogram, which is not parsed further than the ports.
		l4_avail = payload != 0 ? payload : avail - IPV6_HDR_LEN;
		return read_ports(ip + IPV6_HDR_LEN, l4_avail, info);
	}
	return PARSE_IP;
}

enum parse_result
parse_packet(const uint8_t *data, size_t len, struct pkt_info *info)
{
	size_t off = ETH_HDR_LEN;
	uint16_t type;
	int tags = 0;

	memset(info, 0, sizeof(*info));
	if (len < ETH_HDR_LEN)
		return PARSE_MALFORMED;

	type = be16(data + 12);
	while (type == ETHERTYPE_VLAN || type == ETHERTYPE_QINQ) {
		if (tags++ == MAX_VLAN_TAGS || len < off + VLAN_TAG_LEN)
			return PARSE_MALFORMED;
		type = be16(data + off + 2);
		off += VLAN_TAG_LEN;
	}
	info->ether_type = type;

	switch (type) {
	case ETHERTYPE_IPV4:
		return parse_ipv4(data + off, len - off, info);
	case ETHERTYPE_IPV6:
		return parse_ipv6(data + off, len - off, info);
	case ETHERTYPE_ARP:
		return PARSE_ARP;
	default:
		return PARSE_OTHER;
	}
}
