/*
 *  ifupdown for busybox
 *  Copyright (c) 2002 Glenn McGrath <bug1@optushome.com.au>
 *
 *  Based on ifupdown v 0.6.4 by Anthony Towns
 *  Copyright (c) 1999 Anthony Towns <aj@azure.humbug.org.au>
 *
 *  Changes to upstream version
 *  Remove checks for kernel version, assume kernel version 2.2.0 or better.
 *  Lines in the interfaces file cannot wrap.
 *  To adhere to the FHS, the default state file is /var/run/ifstate.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libbb.h"

#define MAX_OPT_DEPTH 10
#define EUNBALBRACK 10001
#define EUNDEFVAR   10002
#define EUNBALPER   10000

typedef struct interface_defn_s interface_defn_t;

typedef int (execfn)(char *command);
typedef int (command_set)(interface_defn_t *ifd, execfn *e);

typedef struct method_s {
	char *name;
	command_set *up;
	command_set *down;
} method_t;

typedef struct address_family_s {
	char *name;
	int n_methods;
	method_t *method;
} address_family_t;

typedef struct mapping_defn_s {
	struct mapping_defn_s *next;

	int max_matches;
	int n_matches;
	char **match;

	char *script;

	int max_mappings;
	int n_mappings;
	char **mapping;
} mapping_defn_t;

typedef struct variable_s {
	char *name;
	char *value;
} variable_t;

struct interface_defn_s {
	struct interface_defn_s *next;

	char *iface;
	address_family_t *address_family;
	method_t *method;

	int automatic;

	int max_options;
	int n_options;
	variable_t *option;
};

typedef struct interfaces_file_s {
	llist_t *autointerfaces;
	interface_defn_t *ifaces;
	mapping_defn_t *mappings;
} interfaces_file_t;

static char no_act = 0;
static char verbose = 0;
static char **environ = NULL;

static void addstr(char **buf, size_t *len, size_t *pos, char *str, size_t str_length)
{
	if (*pos + str_length >= *len) {
		char *newbuf;

		newbuf = xrealloc(*buf, *len * 2 + str_length + 1);
		*buf = newbuf;
		*len = *len * 2 + str_length + 1;
	}

	while (str_length-- >= 1) {
		(*buf)[(*pos)++] = *str;
		str++;
	}
	(*buf)[*pos] = '\0';
}

static int strncmpz(char *l, char *r, size_t llen)
{
	int i = strncmp(l, r, llen);

	if (i == 0) {
		return(-r[llen]);
	} else {
		return(i);
	}
}

static char *get_var(char *id, size_t idlen, interface_defn_t *ifd)
{
	int i;

	if (strncmpz(id, "iface", idlen) == 0) {
		return (ifd->iface);
	} else {
		for (i = 0; i < ifd->n_options; i++) {
			if (strncmpz(id, ifd->option[i].name, idlen) == 0) {
				return (ifd->option[i].value);
			}
		}
	}

	return(NULL);
}

static char *parse(char *command, interface_defn_t *ifd)
{

	char *result = NULL;
	size_t pos = 0, len = 0;
	size_t old_pos[MAX_OPT_DEPTH] = { 0 };
	int okay[MAX_OPT_DEPTH] = { 1 };
	int opt_depth = 1;

	while (*command) {
		switch (*command) {

		default:
			addstr(&result, &len, &pos, command, 1);
			command++;
			break;
		case '\\':
			if (command[1]) {
				addstr(&result, &len, &pos, command + 1, 1);
				command += 2;
			} else {
				addstr(&result, &len, &pos, command, 1);
				command++;
			}
			break;
		case '[':
			if (command[1] == '[' && opt_depth < MAX_OPT_DEPTH) {
				old_pos[opt_depth] = pos;
				okay[opt_depth] = 1;
				opt_depth++;
				command += 2;
			} else {
				addstr(&result, &len, &pos, "[", 1);
				command++;
			}
			break;
		case ']':
			if (command[1] == ']' && opt_depth > 1) {
				opt_depth--;
				if (!okay[opt_depth]) {
					pos = old_pos[opt_depth];
					result[pos] = '\0';
				}
				command += 2;
			} else {
				addstr(&result, &len, &pos, "]", 1);
				command++;
			}
			break;
		case '%':
		{
			char *nextpercent;
			char *varvalue;

			command++;
			nextpercent = strchr(command, '%');
			if (!nextpercent) {
				errno = EUNBALPER;
				free(result);
				return (NULL);
			}

			varvalue = get_var(command, nextpercent - command, ifd);

			if (varvalue) {
				addstr(&result, &len, &pos, varvalue, xstrlen(varvalue));
			} else {
				okay[opt_depth - 1] = 0;
			}

			command = nextpercent + 1;
		}
			break;
		}
	}

	if (opt_depth > 1) {
		errno = EUNBALBRACK;
		free(result);
		return(NULL);
	}

	if (!okay[0]) {
		errno = EUNDEFVAR;
		free(result);
		return(NULL);
	}

	return(result);
}

static int execute(char *command, interface_defn_t *ifd, execfn *exec)
{
	char *out;
	int ret;

	out = parse(command, ifd);
	if (!out) {
		return(0);
	}

	ret = (*exec) (out);

	free(out);
	return(ret);
}

#ifdef CONFIG_FEATURE_IFUPDOWN_IPX
static int static_up_ipx(interface_defn_t *ifd, execfn *exec)
{
	if (!execute("ipx_interface add %iface% %frame% %netnum%", ifd, exec)) {
		return(0);
	}
	return(1);
}

static int static_down_ipx(interface_defn_t *ifd, execfn *exec)
{
	if (!execute("ipx_interface del %iface% %frame%", ifd, exec)) {
		return(0);
	}
	return(1);
}

static int dynamic_up(interface_defn_t *ifd, execfn *exec)
{
	if (!execute("ipx_interface add %iface% %frame%", ifd, exec)) {
		return(0);
	}
	return(1);
}

static int dynamic_down(interface_defn_t *ifd, execfn *exec)
{
	if (!execute("ipx_interface del %iface% %frame%", ifd, exec)) {
		return(0);
	}
	return(1);
}

static method_t methods_ipx[] = {
	{ "dynamic", dynamic_up, dynamic_down, },
	{ "static", static_up_ipx, static_down_ipx, },
};

address_family_t addr_ipx = {
	"ipx",
	sizeof(methods_ipx) / sizeof(method_t),
	methods_ipx
};
#endif /* IFUP_FEATURE_IPX */

#ifdef CONFIG_FEATURE_IFUPDOWN_IPV6
static int loopback_up6(interface_defn_t *ifd, execfn *exec)
{
#ifdef CONFIG_FEATURE_IFUPDOWN_IP
	if (!execute("ip link set %iface% up", ifd, exec))
		return(0);
	if (!execute("ip addr add ::1 dev %iface%", ifd, exec))
		return(0);
#else
	if (!execute("ifconfig %iface% add ::1", ifd, exec))
		return(0);
#endif
	return(1);
}

static int loopback_down6(interface_defn_t *ifd, execfn *exec)
{
#ifdef CONFIG_FEATURE_IFUPDOWN_IP
	if (!execute("ip link set %iface% down", ifd, exec))
		return(0);
#else
	if (!execute("ifconfig %iface% del ::1", ifd, exec))
		return(0);
#endif
	return(1);
}

static int static_up6(interface_defn_t *ifd, execfn *exec)
{
#ifdef CONFIG_FEATURE_IFUPDOWN_IP
	if (!execute("ip link set %iface% up", ifd, exec))
		return(0);
	if (!execute("ip addr add %address%/%netmask% dev %iface%", ifd, exec))
		return(0);
	if (!execute("[[ ip route add ::/0 via %gateway% ]]", ifd, exec))
		return(0);
#else
	if (!execute("ifconfig %iface% [[media %media%]] [[hw %hwaddress%]] [[mtu %mtu%]] up", ifd, exec)) {
		return(0);
	}
	if (!execute("ifconfig %iface% add %address%/%netmask%", ifd, exec)) {
		return(0);
	}
	if (!execute("[[ route -A inet6 add ::/0 gw %gateway% ]]", ifd, exec)) {
		return(0);
	}
#endif
	return(1);
}

static int static_down6(interface_defn_t *ifd, execfn *exec)
{
#ifdef CONFIG_FEATURE_IFUPDOWN_IP
	if (!execute("ip link set %iface% down", ifd, exec))
		return(0);
#else
	if (!execute("ifconfig %iface% down", ifd, exec)) {
		return(0);
	}
#endif
	return(1);
}

#ifdef CONFIG_FEATURE_IFUPDOWN_IP
static int v4tunnel_up(interface_defn_t *ifd, execfn *exec)
{
	if (!execute("ip tunnel add %iface% mode sit remote %endpoint% [[local %local%]] [[ttl %ttl%]]", ifd, exec)) {
		return(0);
	}
	if (!execute("ip link set %iface% up", ifd, exec)) {
		return(0);
	}
	if (!execute("ip addr add %address%/%netmask% dev %iface%", ifd, exec)) {
		return(0);
	}
	if (!execute("[[ ip route add ::/0 via %gateway% ]]", ifd, exec)) {
		return(0);
	}
	return(1);
}

static int v4tunnel_down(interface_defn_t * ifd, execfn * exec)
{
	if (!execute("ip tunnel del %iface%", ifd, exec)) {
		return(0);
	}
	return(1);
}
#endif

static method_t methods6[] = {
#ifdef CONFIG_FEATURE_IFUPDOWN_IP
	{ "v4tunnel", v4tunnel_up, v4tunnel_down, },
#endif
	{ "static", static_up6, static_down6, },
	{ "loopback", loopback_up6, loopback_down6, },
};

address_family_t addr_inet6 = {
	"inet6",
	sizeof(methods6) / sizeof(method_t),
	methods6
};
#endif /* CONFIG_FEATURE_IFUPDOWN_IPV6 */

#ifdef CONFIG_FEATURE_IFUPDOWN_IPV4
static int loopback_up(interface_defn_t *ifd, execfn *exec)
{
#ifdef CONFIG_FEATURE_IFUPDOWN_IP
	if (!execute("ip link set %iface% up", ifd, exec))
		return(0);
	if (!execute("ip addr add 127.0.0.1 dev %iface%", ifd, exec))
		return(0);
#else
	if (!execute("ifconfig %iface% 127.0.0.1 up", ifd, exec)) {
		return(0);
	}
#endif
	return(1);
}

static int loopback_down(interface_defn_t *ifd, execfn *exec)
{
#ifdef CONFIG_FEATURE_IFUPDOWN_IP
	if (!execute("ip -f inet addr flush dev %iface%", ifd, exec))
		return(0);
	if (!execute("ip link set %iface% down", ifd, exec))
		return(0);
#else
	if (!execute("ifconfig %iface% 127.0.0.1 down", ifd, exec)) {
		return(0);
	}
#endif
	return(1);
}

static int static_up(interface_defn_t *ifd, execfn *exec)
{
#ifdef CONFIG_FEATURE_IFUPDOWN_IP
	if (!execute("ip link set %iface% up", ifd, exec))
		return(0);
	if (!execute("ip addr add %address%/%netmask% dev %iface%", ifd, exec))
		return(0);
	if (!execute("[[ ip route add default via %gateway% dev %iface% ]]", ifd, exec))
		return(0);
#else
	if (!execute("ifconfig %iface% %address% netmask %netmask% [[broadcast %broadcast%]] 	[[pointopoint %pointopoint%]] [[media %media%]] [[mtu %mtu%]] 	[[hw %hwaddress%]] up",
		 ifd, exec)) {
		return(0);
	}
	if (!execute("[[ route add default gw %gateway% %iface% ]]", ifd, exec)) {
		return(0);
	}
#endif
	return(1);
}

static int static_down(interface_defn_t *ifd, execfn *exec)
{
#ifdef CONFIG_FEATURE_IFUPDOWN_IP
	if (!execute("[[ ip route del default via %gateway% dev %iface% ]]", ifd, exec))
		return(0);
	if (!execute("ip -f inet addr flush dev %iface%", ifd, exec))
		return(0);
	if (!execute("ip link set %iface% down", ifd, exec))
		return(0);
#else
	if (!execute("[[ route del default gw %gateway% %iface% ]]", ifd, exec)) {
		return(0);
	}
	if (!execute("ifconfig %iface% down", ifd, exec)) {
		return(0);
	}
#endif
	return(1);
}

static int execable(char *program)
{
	struct stat buf;
	if (0 == stat(program, &buf)) {
		if (S_ISREG(buf.st_mode) && (S_IXUSR & buf.st_mode)) {
			return(1);
		}
	}
	return(0);
}

static int dhcp_up(interface_defn_t *ifd, execfn *exec)
{
	if (execable("/sbin/dhclient")) {
		if (!execute("dhclient -pf /var/run/dhclient.%iface%.pid %iface%", ifd, exec)) {
			return(0);
		}
	} else if (execable("/sbin/pump")) {
		if (!execute("pump -i %iface% [[-h %hostname%]] [[-l %leasehours%]]", ifd, exec)) {
			return(0);
		}
	} else if (execable("/sbin/udhcpc")) {
		if (!execute("udhcpc -n -p /var/run/udhcpc.%iface%.pid -i %iface% [[-H %hostname%]] [[-c %clientid%]]", ifd, exec)) {
			return 0;
		}
	} else if (execable("/sbin/dhcpcd")) {
		if (!execute("dhcpcd [[-h %hostname%]] [[-i %vendor%]] [[-I %clientid%]] [[-l %leasetime%]] %iface%", ifd, exec)) {
			return(0);
		}
	}
	return(1);
}

static int dhcp_down(interface_defn_t *ifd, execfn *exec)
{
	if (execable("/sbin/dhclient")) {
		if (!execute("kill -9 `cat /var/run/udhcpc.%iface%.pid`", ifd, exec)) {
			return(0);
		}
	} else if (execable("/sbin/pump")) {
		if (!execute("pump -i %iface% -k", ifd, exec)) {
			return(0);
		}
	} else if (execable("/sbin/udhcpc")) {
		if (!execute("kill -9 `cat /var/run/udhcpc.%iface%.pid`", ifd, exec)) {
			return(0);
		}
	} else if (execable("/sbin/dhcpcd")) {
		if (!execute("dhcpcd -k %iface%", ifd, exec)) {
			return(0);
		}
	}
	if (!execute("ifconfig %iface% down", ifd, exec)) {
		return(0);
	}
	return(1);
}

static int bootp_up(interface_defn_t *ifd, execfn *exec)
{
	if (!execute("bootpc [[--bootfile %bootfile%]] --dev %iface% [[--server %server%]]            [[--hwaddr %hwaddr%]] --returniffail --serverbcast", ifd, exec)) {
		return 0;
	}
	return 1;
}

static int bootp_down(interface_defn_t *ifd, execfn *exec)
{
	if (!execute("ifconfig down %iface%", ifd, exec)) {
		return 0;
	}
	return 1;
}

static int ppp_up(interface_defn_t *ifd, execfn *exec)
{
	if (!execute("pon [[%provider%]]", ifd, exec)) {
		return 0;
	}
	return 1;
}

static int ppp_down(interface_defn_t *ifd, execfn *exec)
{
	if (!execute("poff [[%provider%]]", ifd, exec)) {
		return 0;
	}
	return 1;
}

static int wvdial_up(interface_defn_t *ifd, execfn *exec)
{
	if (!execute("/sbin/start-stop-daemon --start -x /usr/bin/wvdial -p /var/run/wvdial.%iface% -b -m -- [[ %provider% ]]", ifd, exec)) {
		return 0;
	}
	return 1;
}

static int wvdial_down(interface_defn_t *ifd, execfn *exec)
{
	if (!execute ("/sbin/start-stop-daemon --stop -x /usr/bin/wvdial -p /var/run/wvdial.%iface% -s 2", ifd, exec)) {
		return 0;
	}
	return 1;
}

static method_t methods[] = {
	{ "wvdial", wvdial_up, wvdial_down, },
	{ "ppp", ppp_up, ppp_down, },
	{ "static", static_up, static_down, },
	{ "bootp", bootp_up, bootp_down, },
	{ "dhcp", dhcp_up, dhcp_down, },
	{ "loopback", loopback_up, loopback_down, },
};

address_family_t addr_inet = {
	"inet",
	sizeof(methods) / sizeof(method_t),
	methods
};

#endif	/* ifdef CONFIG_FEATURE_IFUPDOWN_IPV4 */

static char *next_word(char **buf)
{
	unsigned short length;
	char *word;

	if ((buf == NULL) || (*buf == NULL) || (**buf == '\0')) {
		return NULL;
	}

	/* Skip over leading whitespace */
	word = *buf + strspn(*buf, " \t\n");

	/* Find the length of this word */
	length = strcspn(word, " \t\n");
	if (length == 0) {
		return(NULL);
	}
	*buf = word + length;
	**buf = '\0';
	(*buf)++;

	return word;
}

static address_family_t *get_address_family(address_family_t *af[], char *name)
{
	int i;

	for (i = 0; af[i]; i++) {
		if (strcmp(af[i]->name, name) == 0) {
			return af[i];
		}
	}
	return NULL;
}

static method_t *get_method(address_family_t *af, char *name)
{
	int i;

	for (i = 0; i < af->n_methods; i++) {
		if (strcmp(af->method[i].name, name) == 0) {
			return &af->method[i];
		}
	}
	return(NULL);
}

static int duplicate_if(interface_defn_t *ifa, interface_defn_t *ifb)
{
	if (strcmp(ifa->iface, ifb->iface) != 0) {
		return(0);
	}
	if (ifa->address_family != ifb->address_family) {
		return(0);
	}
	return(1);
}

static const llist_t *find_list_string(const llist_t *list, const char *string)
{
	while (list) {
		if (strcmp(list->data, string) == 0) {
			return(list);
		}
		list = list->link;
	}
	return(NULL);
}

static interfaces_file_t *read_interfaces(char *filename)
{
#ifdef CONFIG_FEATURE_IFUPDOWN_MAPPING
	mapping_defn_t *currmap = NULL;
#endif
	interface_defn_t *currif = NULL;
	interfaces_file_t *defn;
	FILE *f;
	char *firstword;
	char *buf;

	enum { NONE, IFACE, MAPPING } currently_processing = NONE;

	defn = xmalloc(sizeof(interfaces_file_t));
	defn->autointerfaces = NULL;
	defn->mappings = NULL;
	defn->ifaces = NULL;

	f = xfopen(filename, "r");

	while ((buf = get_line_from_file(f)) != NULL) {
		char *buf_ptr = buf;

		/* Ignore comments */
		if (buf[0] == '#') {
			continue;
		}

		firstword = next_word(&buf_ptr);
		if (firstword == NULL) {
			continue;	/* blank line */
		}

		if (strcmp(firstword, "mapping") == 0) {
#ifdef CONFIG_FEATURE_IFUPDOWN_MAPPING
			currmap = xmalloc(sizeof(mapping_defn_t));
			currmap->max_matches = 0;
			currmap->n_matches = 0;
			currmap->match = NULL;

			while ((firstword = next_word(&buf_ptr)) != NULL) {
				if (currmap->max_matches == currmap->n_matches) {
					currmap->max_matches = currmap->max_matches * 2 + 1;
					currmap->match = xrealloc(currmap->match, sizeof(currmap->match) * currmap->max_matches);
				}

				currmap->match[currmap->n_matches++] = xstrdup(firstword);
			}
			currmap->max_mappings = 0;
			currmap->n_mappings = 0;
			currmap->mapping = NULL;
			currmap->script = NULL;
			{
				mapping_defn_t **where = &defn->mappings;
				while (*where != NULL) {
					where = &(*where)->next;
				}
				*where = currmap;
				currmap->next = NULL;
			}
#endif
			currently_processing = MAPPING;
		} else if (strcmp(firstword, "iface") == 0) {
			{
				char *iface_name;
				char *address_family_name;
				char *method_name;
				address_family_t *addr_fams[] = {
#ifdef CONFIG_FEATURE_IFUPDOWN_IPV4
					&addr_inet,
#endif
#ifdef CONFIG_FEATURE_IFUPDOWN_IPV6
					&addr_inet6,
#endif
#ifdef CONFIG_FEATURE_IFUPDOWN_IPX
					&addr_ipx,
#endif
					NULL
				};

				currif = xmalloc(sizeof(interface_defn_t));
				iface_name = next_word(&buf_ptr);
				address_family_name = next_word(&buf_ptr);
				method_name = next_word(&buf_ptr);

				if (buf_ptr == NULL) {
					error_msg("too few parameters for line \"%s\"", buf);
					return NULL;
				}

				if (buf_ptr[0] != '\0') {
					error_msg("too many parameters \"%s\"", buf);
					return NULL;
				}

				currif->iface = xstrdup(iface_name);

				currif->address_family = get_address_family(addr_fams, address_family_name);
				if (!currif->address_family) {
					error_msg("unknown address type \"%s\"", buf);
					return NULL;
				}

				currif->method = get_method(currif->address_family, method_name);
				if (!currif->method) {
					error_msg("unknown method \"%s\"", buf);
					return NULL;
				}

				currif->automatic = 1;
				currif->max_options = 0;
				currif->n_options = 0;
				currif->option = NULL;


				{
					interface_defn_t **where = &defn->ifaces;

					while (*where != NULL) {
						if (duplicate_if(*where, currif)) {
							error_msg("duplicate interface \"%s\"", buf);
							return NULL;
						}
						where = &(*where)->next;
					}

					*where = currif;
					currif->next = NULL;
				}
			}
			currently_processing = IFACE;
		} else if (strcmp(firstword, "auto") == 0) {
			while ((firstword = next_word(&buf_ptr)) != NULL) {

				/* Check the interface isnt already listed */
				if (find_list_string(defn->autointerfaces, firstword)) {
					perror_msg_and_die("interface declared auto twice \"%s\"", buf);
				}

				/* Add the interface to the list */
				defn->autointerfaces = llist_add_to(defn->autointerfaces, strdup(firstword));
			}
			currently_processing = NONE;
		} else {
			switch (currently_processing) {
			case IFACE:
			{
				int i;

				if (xstrlen(buf_ptr) == 0) {
					error_msg("option with empty value \"%s\"", buf);
					return NULL;
				}

				if (strcmp(firstword, "up") != 0
					&& strcmp(firstword, "down") != 0
					&& strcmp(firstword, "pre-up") != 0
					&& strcmp(firstword, "post-down") != 0) {
					for (i = 0; i < currif->n_options; i++) {
						if (strcmp(currif->option[i].name, firstword) == 0) {
							error_msg("duplicate option \"%s\"", buf);
							return NULL;
						}
					}
				}
			}
				if (currif->n_options >= currif->max_options) {
					variable_t *opt;

					currif->max_options = currif->max_options + 10;
					opt = xrealloc(currif->option, sizeof(*opt) * currif->max_options);
					currif->option = opt;
				}
				currif->option[currif->n_options].name = xstrdup(firstword);
				currif->option[currif->n_options].value = xstrdup(next_word(&buf_ptr));
				if (!currif->option[currif->n_options].name) {
					perror(filename);
					return NULL;
				}
				if (!currif->option[currif->n_options].value) {
					perror(filename);
					return NULL;
				}
				currif->n_options++;
				break;
			case MAPPING:
#ifdef CONFIG_FEATURE_IFUPDOWN_MAPPING
				if (strcmp(firstword, "script") == 0) {
					if (currmap->script != NULL) {
						error_msg("duplicate script in mapping \"%s\"", buf);
						return NULL;
					} else {
						currmap->script = xstrdup(next_word(&buf_ptr));
					}
				} else if (strcmp(firstword, "map") == 0) {
					if (currmap->max_mappings == currmap->n_mappings) {
						currmap->max_mappings = currmap->max_mappings * 2 + 1;
						currmap->mapping = xrealloc(currmap->mapping, sizeof(char *) * currmap->max_mappings);
					}
					currmap->mapping[currmap->n_mappings] = xstrdup(next_word(&buf_ptr));
					currmap->n_mappings++;
				} else {
					error_msg("misplaced option \"%s\"", buf);
					return NULL;
				}
#endif
				break;
			case NONE:
			default:
				error_msg("misplaced option \"%s\"", buf);
				return NULL;
			}
		}
		free(buf);
	}
	if (ferror(f) != 0) {
		perror_msg_and_die("%s", filename);
	}
	fclose(f);

	return defn;
}

static int check(char *str)
{
	return (str != NULL);
}

static char *setlocalenv(char *format, char *name, char *value)
{
	char *result;
	char *here;
	char *there;

	result = xmalloc(xstrlen(format) + xstrlen(name) + xstrlen(value) + 1);

	sprintf(result, format, name, value);

	for (here = there = result; *there != '=' && *there; there++) {
		if (*there == '-')
			*there = '_';
		if (isalpha(*there))
			*there = toupper(*there);

		if (isalnum(*there) || *there == '_') {
			*here = *there;
			here++;
		}
	}
	memmove(here, there, xstrlen(there) + 1);

	return result;
}

static void set_environ(interface_defn_t *iface, char *mode)
{
	char **environend;
	int i;
	const int n_env_entries = iface->n_options + 5;
	char **ppch;

	if (environ != NULL) {
		for (ppch = environ; *ppch; ppch++) {
			free(*ppch);
			*ppch = NULL;
		}
		free(environ);
		environ = NULL;
	}
	environ = xmalloc(sizeof(char *) * (n_env_entries + 1 /* for final NULL */ ));
	environend = environ;
	*environend = NULL;

	for (i = 0; i < iface->n_options; i++) {
		if (strcmp(iface->option[i].name, "up") == 0
			|| strcmp(iface->option[i].name, "down") == 0
			|| strcmp(iface->option[i].name, "pre-up") == 0
			|| strcmp(iface->option[i].name, "post-down") == 0) {
			continue;
		}
		*(environend++) = setlocalenv("IF_%s=%s", iface->option[i].name, iface->option[i].value);
		*environend = NULL;
	}

	*(environend++) = setlocalenv("%s=%s", "IFACE", iface->iface);
	*environend = NULL;
	*(environend++) = setlocalenv("%s=%s", "ADDRFAM", iface->address_family->name);
	*environend = NULL;
	*(environend++) = setlocalenv("%s=%s", "METHOD", iface->method->name);
	*environend = NULL;
	*(environend++) = setlocalenv("%s=%s", "MODE", mode);
	*environend = NULL;
	*(environend++) = setlocalenv("%s=%s", "PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
	*environend = NULL;
}

static int doit(char *str)
{
	if (verbose || no_act) {
		error_msg("%s", str);
	}
	if (!no_act) {
		pid_t child;
		int status;

		fflush(NULL);
		switch (child = fork()) {
		case -1:		/* failure */
			return 0;
		case 0:		/* child */
			execle("/bin/sh", "/bin/sh", "-c", str, NULL, environ);
			exit(127);
		}
		waitpid(child, &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			return 0;
		}
	}
	return (1);
}

static int execute_all(interface_defn_t *ifd, execfn *exec, const char *opt)
{
	int i;
	char *buf;

	for (i = 0; i < ifd->n_options; i++) {
		if (strcmp(ifd->option[i].name, opt) == 0) {
			if (!(*exec) (ifd->option[i].value)) {
				return 0;
			}
		}
	}

	buf = xmalloc(xstrlen(opt) + 19);
	sprintf(buf, "/etc/network/if-%s.d", opt);
	run_parts(&buf, 0);
	free(buf);
	return (1);
}

static int iface_up(interface_defn_t *iface)
{
	if (!iface->method->up(iface, check)) {
		return (-1);
	}

	set_environ(iface, "start");
	if (!execute_all(iface, doit, "pre-up")) {
		return (0);
	}
	if (!iface->method->up(iface, doit)) {
		return (0);
	}
	if (!execute_all(iface, doit, "up")) {
		return (0);
	}

	return (1);
}

static int iface_down(interface_defn_t *iface)
{
	if (!iface->method->down(iface, check)) {
		return (-1);
	}
	set_environ(iface, "stop");
	if (!execute_all(iface, doit, "down")) {
		return (0);
	}
	if (!iface->method->down(iface, doit)) {
		return (0);
	}
	if (!execute_all(iface, doit, "post-down")) {
		return (0);
	}
	return (1);
}

#ifdef CONFIG_FEATURE_IFUPDOWN_MAPPING
static int popen2(FILE **in, FILE **out, char *command, ...)
{
	va_list ap;
	char *argv[11] = { command };
	int argc;
	int infd[2], outfd[2];
	pid_t pid;

	argc = 1;
	va_start(ap, command);
	while ((argc < 10) && (argv[argc] = va_arg(ap, char *))) {
		argc++;
	}
	argv[argc] = NULL;	/* make sure */
	va_end(ap);

	if (pipe(infd) != 0) {
		return 0;
	}

	if (pipe(outfd) != 0) {
		close(infd[0]);
		close(infd[1]);
		return 0;
	}

	fflush(NULL);
	switch (pid = fork()) {
	case -1:			/* failure */
		close(infd[0]);
		close(infd[1]);
		close(outfd[0]);
		close(outfd[1]);
		return 0;
	case 0:			/* child */
		dup2(infd[0], 0);
		dup2(outfd[1], 1);
		close(infd[0]);
		close(infd[1]);
		close(outfd[0]);
		close(outfd[1]);
		execvp(command, argv);
		exit(127);
	default:			/* parent */
		*in = fdopen(infd[1], "w");
		*out = fdopen(outfd[0], "r");
		close(infd[0]);
		close(outfd[1]);
		return pid;
	}
	/* unreached */
}

static int run_mapping(char *physical, char *logical, int len, mapping_defn_t * map)
{
	FILE *in, *out;
	int i, status;
	pid_t pid;


	pid = popen2(&in, &out, map->script, physical, NULL);
	if (pid == 0) {
		return 0;
	}
	for (i = 0; i < map->n_mappings; i++) {
		fprintf(in, "%s\n", map->mapping[i]);
	}
	fclose(in);
	waitpid(pid, &status, 0);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		if (fgets(logical, len, out)) {
			char *pch = logical + xstrlen(logical) - 1;

			while (pch >= logical && isspace(*pch))
				*(pch--) = '\0';
		}
	}
	fclose(out);

	return 1;
}
#endif /* CONFIG_FEATURE_IFUPDOWN_IPV6 */

static llist_t *find_iface_state(llist_t *state_list, const char *iface)
{
	unsigned short iface_len = xstrlen(iface);
	llist_t *search = state_list;

	while (search) {
		if ((strncmp(search->data, iface, iface_len) == 0) &&
			(search->data[iface_len] == '=')) {
			return(search);
		}
		search = search->link;
	}
	return(NULL);
}

extern int ifupdown_main(int argc, char **argv)
{
	int (*cmds) (interface_defn_t *) = NULL;
	interfaces_file_t *defn;
	FILE *state_fp = NULL;
	llist_t *state_list = NULL;
	llist_t *target_list = NULL;
	char *interfaces = "/etc/network/interfaces";
	const char *statefile = "/var/run/ifstate";

#ifdef CONFIG_FEATURE_IFUPDOWN_MAPPING
	int run_mappings = 1;
#endif
	int do_all = 0;
	int force = 0;
	int i;

	if (applet_name[2] == 'u') {
		/* ifup command */
		cmds = iface_up;
	} else {
		/* ifdown command */
		cmds = iface_down;
	}

#ifdef CONFIG_FEATURE_IFUPDOWN_MAPPING
	while ((i = getopt(argc, argv, "i:hvnamf")) != -1) {
#else
	while ((i = getopt(argc, argv, "i:hvnaf")) != -1) {
#endif
		switch (i) {
		case 'i':	/* interfaces */
			interfaces = xstrdup(optarg);
			break;
		case 'v':	/* verbose */
			verbose = 1;
			break;
		case 'a':	/* all */
			do_all = 1;
			break;
		case 'n':	/* no-act */
			no_act = 1;
			break;
#ifdef CONFIG_FEATURE_IFUPDOWN_MAPPING
		case 'm':	/* no-mappings */
			run_mappings = 0;
			break;
#endif
		case 'f':	/* force */
			force = 1;
			break;
		default:
			show_usage();
			break;
		}
	}

	if (argc - optind > 0) {
		if (do_all) {
			show_usage();
		}
	} else {
		if (!do_all) {
			show_usage();
		}
	}			

	defn = read_interfaces(interfaces);

	if (no_act) {
		state_fp = fopen(statefile, "r");
	} else {
		state_fp = xfopen(statefile, "a+");
	}

	/* Read the previous state from the state file */
	if (state_fp != NULL) {
		char *start;
		while ((start = get_line_from_file(state_fp)) != NULL) {
			char *end_ptr;
			/* We should only need to check for a single character */
			end_ptr = start + strcspn(start, " \t\n");
			*end_ptr = '\0';
			state_list = llist_add_to(state_list, start);
		}
	}

	/* Create a list of interfaces to work on */
	if (do_all) {
		if (cmds == iface_up) {
			target_list = defn->autointerfaces;
		} else if (cmds == iface_down) {
			const llist_t *list = state_list;
			while (list) {
				target_list = llist_add_to(target_list, strdup(list->data));
				list = list->link;
			}
		}
	} else {
		target_list = llist_add_to(target_list, argv[optind]);
	}


	/* Update the interfaces */
	while (target_list) {
		interface_defn_t *currif;
		char *iface;
		char *liface;
		char *pch;
		int okay = 0;

		iface = strdup(target_list->data);
		target_list = target_list->link;

		pch = strchr(iface, '=');
		if (pch) {
			*pch = '\0';
			liface = strdup(pch + 1);
		} else {
			liface = strdup(iface);
		}

		if (!force) {
			const llist_t *iface_state = find_iface_state(state_list, iface);

			if (cmds == iface_up) {
				/* ifup */
				if (iface_state) {
					error_msg("interface %s already configured", iface);
					continue;
				}
			} else {
				/* ifdown */
				if (iface_state == NULL) {
					error_msg("interface %s not configured", iface);
					continue;
				}
				pch = strchr(iface_state->data, '=');
				liface = strdup(pch + 1);
			}
		}

#ifdef CONFIG_FEATURE_IFUPDOWN_MAPPING
		if ((cmds == iface_up) && run_mappings) {
			mapping_defn_t *currmap;

			for (currmap = defn->mappings; currmap; currmap = currmap->next) {

				for (i = 0; i < currmap->n_matches; i++) {
					if (fnmatch(currmap->match[i], liface, 0) != 0)
						continue;
					if (verbose) {
						error_msg("Running mapping script %s on %s", currmap->script, liface);
					}
					run_mapping(iface, liface, sizeof(liface), currmap);
					break;
				}
			}
		}
#endif

		for (currif = defn->ifaces; currif; currif = currif->next) {
			if (strcmp(liface, currif->iface) == 0) {
				char *oldiface = currif->iface;

				okay = 1;
				currif->iface = iface;

				if (verbose) {
					error_msg("Configuring interface %s=%s (%s)", iface, liface, currif->address_family->name);
				}

				/* Call the cmds function pointer, does either iface_up() or iface_down() */
				if (cmds(currif) == -1) {
					printf
						("Don't seem to be have all the variables for %s/%s.\n",
						 liface, currif->address_family->name);
				}

				currif->iface = oldiface;
			}
		}

		if (!okay && !force) {
			error_msg("Ignoring unknown interface %s=%s.", iface, liface);
		} else {
			llist_t *iface_state = find_iface_state(state_list, iface);

			if (cmds == iface_up) {
				char *newiface = xmalloc(xstrlen(iface) + 1 + xstrlen(liface) + 1);
				sprintf(newiface, "%s=%s", iface, liface);
				if (iface_state == NULL) {
					state_list = llist_add_to(state_list, newiface);
				} else {
					free(iface_state->data);
					iface_state->data = newiface;
				}
			} else if (cmds == iface_down) {
				/* Remove an interface from the linked list */
				if (iface_state) {
					/* This needs to be done better */
					free(iface_state->data);
					free(iface_state->link);
					if (iface_state->link) {
						iface_state->data = iface_state->link->data;
						iface_state->link = iface_state->link->link;
					} else {
						iface_state->data = NULL;
						iface_state->link = NULL;
					}						
				}
			}
		}
	}

	/* Actually write the new state */
	if (state_fp != NULL && !no_act) {
		if (ftruncate(fileno(state_fp), 0) < 0) {
			error_msg_and_die("failed to truncate statefile %s: %s", statefile, strerror(errno));
		}

		rewind(state_fp);

		while (state_list) {
			if (state_list->data) {
				fputs(state_list->data, state_fp);
				fputc('\n', state_fp);
			}
			state_list = state_list->link;
		}
		fflush(state_fp);
	}

	/* Cleanup */
	if (state_fp != NULL) {
		fclose(state_fp);
		state_fp = NULL;
	}

	return 0;
}
