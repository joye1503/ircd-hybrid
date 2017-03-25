/*
 *  ircd-hybrid: an advanced, lightweight Internet Relay Chat Daemon (ircd)
 *
 *  Copyright (c) 2004-2017 ircd-hybrid development team
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
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 *  USA
 */

/*! \file m_etrace.c
 * \brief Includes required functions for processing the ETRACE command.
 * \version $Id$
 */

#include "stdinc.h"
#include "list.h"
#include "client.h"
#include "hash.h"
#include "irc_string.h"
#include "ircd.h"
#include "numeric.h"
#include "server.h"
#include "send.h"
#include "parse.h"
#include "modules.h"
#include "conf.h"


/* report_this_status()
 *
 * inputs       - pointer to client to report to
 *              - pointer to client to report about
 *              - flag full etrace or not
 * output       - NONE
 * side effects - NONE
 */
static void
report_this_status(struct Client *source_p, const struct Client *target_p)
{
  if (target_p->status != STAT_CLIENT)
    return;

  sendto_one_numeric(source_p, &me, RPL_ETRACE,
                     HasUMode(target_p, UMODE_OPER) ? "Oper" : "User",
                     get_client_class(&target_p->connection->confs),
                     target_p->name,
                     target_p->username,
                     target_p->host,
                     target_p->sockhost,
                     target_p->info);
}

/*
 * do_etrace()
 */
static void
do_etrace(struct Client *source_p, int parc, char *parv[])
{
  const char *tname = NULL;
  unsigned int wilds = 0, do_all = 0;
  dlink_node *node;

  sendto_realops_flags(UMODE_SPY, L_ALL, SEND_NOTICE,
                       "ETRACE requested by %s (%s@%s) [%s]",
                       source_p->name, source_p->username,
                       source_p->host, source_p->servptr->name);

  if (EmptyString(parv[1]))
  {
    do_all = 1;
    tname = "*";
  }
  else
  {
    tname = parv[1];
    wilds = has_wildcards(tname);
  }

  if (!wilds && !do_all)
  {
    const struct Client *target_p = find_person(source_p, tname);

    if (target_p && MyConnect(target_p))
      report_this_status(source_p, target_p);

    sendto_one_numeric(source_p, &me, RPL_TRACEEND, me.name);
    return;
  }

  DLINK_FOREACH(node, local_client_list.head)
  {
    const struct Client *target_p = node->data;

    if (!wilds || !match(tname, target_p->name))
      report_this_status(source_p, target_p);
  }

  sendto_one_numeric(source_p, &me, RPL_TRACEEND, me.name);
}

/*! \brief ETRACE command handler
 *
 * \param source_p Pointer to allocated Client struct from which the message
 *                 originally comes from.  This can be a local or remote client.
 * \param parc     Integer holding the number of supplied arguments.
 * \param parv     Argument vector where parv[0] .. parv[parc-1] are non-NULL
 *                 pointers.
 * \note Valid arguments for this command are:
 *      - parv[0] = command
 *      - parv[1] = nick name to trace
 *      - parv[2] = nick or server name to forward the etrace to
 */
static int
mo_etrace(struct Client *source_p, int parc, char *parv[])
{
  if (parc > 2)
    if (server_hunt(source_p, ":%s ETRACE %s :%s", 2, parc, parv)->ret != HUNTED_ISME)
      return 0;

  const struct server_hunt *hunt = server_hunt(source_p, ":%s ETRACE :%s", 1, parc, parv);
  switch (hunt->ret)
  {
    case HUNTED_PASS:
      sendto_one_numeric(source_p, &me, RPL_TRACELINK,
                         ircd_version, hunt->target_p->name, hunt->target_p->from->name);
      break;
    case HUNTED_ISME:
      do_etrace(source_p, parc, parv);
      break;
    default:
      break;
  }

  return 0;
}

static struct Message etrace_msgtab =
{
  .cmd = "ETRACE",
  .args_max = MAXPARA,
  .handlers[UNREGISTERED_HANDLER] = m_unregistered,
  .handlers[CLIENT_HANDLER] = m_not_oper,
  .handlers[SERVER_HANDLER] = mo_etrace,
  .handlers[ENCAP_HANDLER] = m_ignore,
  .handlers[OPER_HANDLER] = mo_etrace
};

static void
module_init(void)
{
  mod_add_cmd(&etrace_msgtab);
}

static void
module_exit(void)
{
  mod_del_cmd(&etrace_msgtab);
}

struct module module_entry =
{
  .version = "$Revision$",
  .modinit = module_init,
  .modexit = module_exit,
};
