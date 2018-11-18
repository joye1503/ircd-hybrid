/*
 *  ircd-hybrid: an advanced, lightweight Internet Relay Chat Daemon (ircd)
 *
 *  Copyright (c) 1997-2018 ircd-hybrid development team
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

/*! \file m_sjoin.c
 * \brief Includes required functions for processing the SJOIN command.
 * \version $Id$
 */

#include "stdinc.h"
#include "list.h"
#include "channel.h"
#include "channel_mode.h"
#include "client.h"
#include "hash.h"
#include "irc_string.h"
#include "ircd.h"
#include "numeric.h"
#include "send.h"
#include "parse.h"
#include "modules.h"
#include "server.h"
#include "conf.h"
#include "misc.h"


static void set_final_mode(struct Mode *, struct Mode *, char *, char *);
static void remove_our_modes(struct Channel *, struct Client *);
static void remove_a_mode(struct Channel *, struct Client *, int, char);
static void remove_ban_list(struct Channel *, struct Client *, dlink_list *, char);


/* ms_sjoin()
 *
 * parv[0] - command
 * parv[1] - TS
 * parv[2] - channel
 * parv[3] - modes + n arguments (key and/or limit)
 * parv[4+n] - flags+nick list (all in one parameter)
 *
 * process a SJOIN, taking the TS's into account to either ignore the
 * incoming modes or undo the existing ones or merge them, and JOIN
 * all the specified users while sending JOIN/MODEs to local clients
 */
static int
ms_sjoin(struct Client *source_p, int parc, char *parv[])
{
  struct Channel *chptr = NULL;
  struct Client  *target_p = NULL;
  uintmax_t newts;
  uintmax_t oldts;
  uintmax_t tstosend;
  struct Mode mode, *oldmode;
  int            args = 0;
  bool           isnew = false;
  bool           keep_our_modes = true;
  bool           keep_new_modes = true;
  bool           have_many_uids = false;
  unsigned int   lcount;
  char           uid_prefix[CMEMBER_STATUS_FLAGS_LEN + 1];
  char           *up = NULL;
  int            len_uid = 0;
  int            buflen = 0;
  int            slen;
  unsigned       int fl;
  char           *s;
  char           *sptr;
  char uid_buf[IRCD_BUFSIZE];  /* buffer for modes/prefixes */
  char           *uid_ptr;
  char           *p; /* pointer used making sjbuf */
  const char *servername = (ConfigServerHide.hide_servers || IsHidden(source_p)) ?
                            me.name : source_p->name;
  const char *para[MAXMODEPARAMS];
  char sendbuf[MODEBUFLEN] = "";
  char modebuf[MODEBUFLEN] = "";
  char parabuf[MODEBUFLEN] = "";
  unsigned int pargs = 0;

  if (!IsServer(source_p))
    return 0;

  if (channel_check_name(parv[2], false) == false)
  {
    sendto_realops_flags(UMODE_DEBUG, L_ALL, SEND_NOTICE,
                         "*** Too long or invalid channel name from %s(via %s): %s",
                         source_p->name, source_p->from->name, parv[2]);
    return 0;
  }

  pargs = 0;
  newts = strtoumax(parv[1], NULL, 10);

  mode.mode = 0;
  mode.limit = 0;
  mode.key[0] = '\0';

  for (const char *modes = parv[3]; *modes; ++modes)
  {
    switch (*modes)
    {
      case 'k':
        strlcpy(mode.key, parv[4 + args], sizeof(mode.key));
        ++args;

        if (parc < 5 + args)
          return 0;
        break;

      case 'l':
        mode.limit = atoi(parv[4 + args]);
        ++args;

        if (parc < 5 + args)
          return 0;
        break;

      default:
      {
        const struct chan_mode *cmode = cmode_map[(unsigned char)*modes];

        if (cmode)
          mode.mode |= cmode->mode;
        break;
      }
    }
  }

  if ((chptr = hash_find_channel(parv[2])) == NULL)
  {
    isnew = true;
    chptr = channel_make(parv[2]);
  }

  oldts = chptr->creationtime;
  oldmode = &chptr->mode;

  if (ConfigGeneral.ignore_bogus_ts)
  {
    if (newts < 800000000)
    {
      sendto_realops_flags(UMODE_DEBUG, L_ALL, SEND_NOTICE,
                           "*** Bogus TS %ju on %s ignored from %s(via %s)",
                           newts, chptr->name,
                           source_p->name, source_p->from->name);

      newts = (oldts == 0) ? 0 : 800000000;
    }
  }
  else
  {
    if (newts == 0 && isnew == false && oldts)
    {
      sendto_channel_local(NULL, chptr, 0, 0, 0,
                           ":%s NOTICE %s :*** Notice -- TS for %s changed from %ju to 0",
                           me.name, chptr->name, chptr->name, oldts);
      sendto_realops_flags(UMODE_SERVNOTICE, L_ALL, SEND_NOTICE,
                           "Server %s changing TS on %s from %ju to 0",
                           source_p->name, chptr->name, oldts);
    }
  }

  if (isnew == true)
    chptr->creationtime = tstosend = newts;
  else if (newts == 0 || oldts == 0)
    chptr->creationtime = tstosend = 0;
  else if (newts == oldts)
    tstosend = oldts;
  else if (newts < oldts)
  {
    keep_our_modes = false;
    chptr->creationtime = tstosend = newts;
  }
  else
  {
    keep_new_modes = false;
    tstosend = oldts;
  }

  if (keep_new_modes == false)
    mode = *oldmode;
  else if (keep_our_modes == true)
  {
    mode.mode |= oldmode->mode;

    if (oldmode->limit > mode.limit)
      mode.limit = oldmode->limit;
    if (strcmp(mode.key, oldmode->key) < 0)
      strlcpy(mode.key, oldmode->key, sizeof(mode.key));
  }

  set_final_mode(&mode, oldmode, modebuf, parabuf);
  chptr->mode = mode;

  /* Lost the TS, other side wins, so remove modes on this side */
  if (keep_our_modes == false)
  {
    /* Update channel name to be the correct case */
    if (isnew == false)
      strlcpy(chptr->name, parv[2], sizeof(chptr->name));

    remove_our_modes(chptr, source_p);

    remove_ban_list(chptr, source_p, &chptr->banlist, 'b');
    remove_ban_list(chptr, source_p, &chptr->exceptlist, 'e');
    remove_ban_list(chptr, source_p, &chptr->invexlist, 'I');

    clear_ban_cache_list(&chptr->members_local);
    clear_invite_list(&chptr->invites);

    if (chptr->topic[0])
    {
      channel_set_topic(chptr, "", "", 0, false);
      sendto_channel_local(NULL, chptr, 0, 0, 0, ":%s TOPIC %s :",
                           (IsHidden(source_p) ||
                           ConfigServerHide.hide_servers) ?
                           me.name : source_p->name, chptr->name);
    }

    sendto_channel_local(NULL, chptr, 0, 0, 0,
                         ":%s NOTICE %s :*** Notice -- TS for %s changed from %ju to %ju",
                         me.name, chptr->name, chptr->name,
                         oldts, newts);
  }

  if (*modebuf)
  {
    /* This _SHOULD_ be to ALL_MEMBERS
     * It contains only +imnpstlk, etc */
    sendto_channel_local(NULL, chptr, 0, 0, 0, ":%s MODE %s %s %s",
                         servername, chptr->name, modebuf, parabuf);
  }

  if (*parv[3] != '0' && keep_new_modes == true)
    channel_modes(chptr, source_p, modebuf, parabuf);
  else
  {
    modebuf[0] = '0';
    modebuf[1] = '\0';
  }

  buflen = snprintf(uid_buf, sizeof(uid_buf), ":%s SJOIN %ju %s %s %s:",
                    source_p->id, tstosend,
                    chptr->name, modebuf, parabuf);
  uid_ptr = uid_buf + buflen;

  /*
   * Check we can fit a nick on the end, as well as \r\n and a prefix "
   * @%+", and a space.
   */
  if (buflen >= (IRCD_BUFSIZE - IDLEN - 2 - CMEMBER_STATUS_FLAGS_LEN - 1))
  {
    sendto_realops_flags(UMODE_SERVNOTICE, L_ALL, SEND_NOTICE,
                         "Long SJOIN from server: %s(via %s) (ignored)",
                         source_p->name, source_p->from->name);
    return 0;
  }

  char *mbuf = modebuf;
  *mbuf++ = '+';

  s = parv[args + 4];
  while (*s == ' ')
    ++s;

  if ((p = strchr(s, ' ')))
  {
    *p++ = '\0';

    while (*p == ' ')
      ++p;
    have_many_uids = *p != '\0';
  }

  while (*s)
  {
    int valid_mode = 1;
    fl = 0;

    do
    {
      switch (*s)
      {
        case '@':
          fl |= CHFL_CHANOP;
          ++s;
          break;
        case '%':
          fl |= CHFL_HALFOP;
          ++s;
          break;
        case '+':
          fl |= CHFL_VOICE;
          ++s;
          break;
        default:
          valid_mode = 0;
          break;
      }
    } while (valid_mode);

    /*
     * If the client doesn't exist, or if it's fake direction/server, skip.
     * we cannot send ERR_NOSUCHNICK here because if it's a UID, we cannot
     * lookup the nick, and it's better to never send the numeric than only
     * sometimes.
     */
    if ((target_p = find_person(source_p, s)) == NULL || target_p->from != source_p->from)
      goto nextnick;

    len_uid = strlen(target_p->id);
    up = uid_prefix;

    if (keep_new_modes == true)
    {
      if (fl & CHFL_CHANOP)
      {
        *up++  = '@';
        len_uid++;
      }

      if (fl & CHFL_HALFOP)
      {
        *up++  = '%';
        len_uid++;
      }

      if (fl & CHFL_VOICE)
      {
        *up++  = '+';
        len_uid++;
      }
    }
    else
      fl = 0;

    *up = '\0';

    if ((uid_ptr - uid_buf + len_uid) > (IRCD_BUFSIZE - 2))
    {
      sendto_server(source_p, 0, 0, "%s", uid_buf);

      buflen = snprintf(uid_buf, sizeof(uid_buf), ":%s SJOIN %ju %s %s %s:",
                        source_p->id, tstosend,
                        chptr->name, modebuf, parabuf);
      uid_ptr = uid_buf + buflen;
    }

    uid_ptr += sprintf(uid_ptr, "%s%s ", uid_prefix, target_p->id);

    if (!IsMember(target_p, chptr))
    {
      add_user_to_channel(chptr, target_p, fl, have_many_uids == false);

      sendto_channel_local(NULL, chptr, 0, CAP_EXTENDED_JOIN, 0, ":%s!%s@%s JOIN %s %s :%s",
                           target_p->name, target_p->username,
                           target_p->host, chptr->name, target_p->account, target_p->info);
      sendto_channel_local(NULL, chptr, 0, 0, CAP_EXTENDED_JOIN, ":%s!%s@%s JOIN :%s",
                           target_p->name, target_p->username,
                           target_p->host, chptr->name);

      if (target_p->away[0])
        sendto_channel_local(target_p, chptr, 0, CAP_AWAY_NOTIFY, 0,
                             ":%s!%s@%s AWAY :%s",
                             target_p->name, target_p->username,
                             target_p->host, target_p->away);
    }

    if (fl & CHFL_CHANOP)
    {
      *mbuf++ = 'o';
      para[pargs++] = target_p->name;

      if (pargs >= MAXMODEPARAMS)
      {
        /*
         * Ok, the code is now going to "walk" through
         * sendbuf, filling in para strings. So, I will use sptr
         * to point into the sendbuf.
         * Notice, that ircsprintf() returns the number of chars
         * successfully inserted into string.
         * - Dianora
         */

        sptr = sendbuf;
        *mbuf = '\0';

        for (lcount = 0; lcount < MAXMODEPARAMS; ++lcount)
        {
          slen = sprintf(sptr, " %s", para[lcount]);  /* see? */
          sptr += slen;  /* ready for next */
        }

        sendto_channel_local(NULL, chptr, 0, 0, 0, ":%s MODE %s %s%s",
                             servername, chptr->name, modebuf, sendbuf);
        mbuf = modebuf;
        *mbuf++ = '+';

        sendbuf[0] = '\0';
        pargs = 0;
      }
    }

    if (fl & CHFL_HALFOP)
    {
      *mbuf++ = 'h';
      para[pargs++] = target_p->name;

      if (pargs >= MAXMODEPARAMS)
      {
        sptr = sendbuf;
        *mbuf = '\0';

        for (lcount = 0; lcount < MAXMODEPARAMS; ++lcount)
        {
          slen = sprintf(sptr, " %s", para[lcount]);
          sptr += slen;
        }

        sendto_channel_local(NULL, chptr, 0, 0, 0, ":%s MODE %s %s%s",
                             servername, chptr->name, modebuf, sendbuf);

        mbuf = modebuf;
        *mbuf++ = '+';

        sendbuf[0] = '\0';
        pargs = 0;
      }
    }

    if (fl & CHFL_VOICE)
    {
      *mbuf++ = 'v';
      para[pargs++] = target_p->name;

      if (pargs >= MAXMODEPARAMS)
      {
        sptr = sendbuf;
        *mbuf = '\0';

        for (lcount = 0; lcount < MAXMODEPARAMS; ++lcount)
        {
          slen = sprintf(sptr, " %s", para[lcount]);
          sptr += slen;
        }

        sendto_channel_local(NULL, chptr, 0, 0, 0, ":%s MODE %s %s%s",
                             servername, chptr->name, modebuf, sendbuf);

        mbuf = modebuf;
        *mbuf++ = '+';

        sendbuf[0] = '\0';
        pargs = 0;
      }
    }

  nextnick:
    if ((s = p) == NULL)
      break;

    while (*s == ' ')
      ++s;

    if ((p = strchr(s, ' ')))
    {
      *p++ = '\0';

      while (*p == ' ')
        ++p;
    }
  }

  *mbuf = '\0';
  *(uid_ptr - 1) = '\0';

  /*
   * checking for lcount < MAXMODEPARAMS at this time is wrong
   * since the code has already verified above that pargs < MAXMODEPARAMS
   * checking for para[lcount] != '\0' is also wrong, since
   * there is no place where para[lcount] is set!
   * - Dianora
   */

  if (pargs)
  {
    sptr = sendbuf;

    for (lcount = 0; lcount < pargs; ++lcount)
    {
      slen = sprintf(sptr, " %s", para[lcount]);
      sptr += slen;
    }

    sendto_channel_local(NULL, chptr, 0, 0, 0, ":%s MODE %s %s%s",
                         servername, chptr->name, modebuf, sendbuf);
  }

  /*
   * If this happens, it's the result of a malformed SJOIN
   * a remnant from the old persistent channel code. *sigh*
   * Or it could be the result of a client just leaving
   * and leaving us with a channel formed just as the client parts.
   * - Dianora
   */
  if (dlink_list_length(&chptr->members) == 0 && isnew == true)
  {
    channel_free(chptr);
    return 0;
  }

  if (*parv[4 + args] == '\0')
    return 0;

  sendto_server(source_p, 0, 0, "%s", uid_buf);
  return 0;
}

/* set_final_mode
 *
 * inputs	- channel mode
 *		- old channel mode
 * output	- NONE
 * side effects	- walk through all the channel modes turning off modes
 *		  that were on in oldmode but aren't on in mode.
 *		  Then walk through turning on modes that are on in mode
 *		  but were not set in oldmode.
 */
static void
set_final_mode(struct Mode *mode, struct Mode *oldmode, char *mbuf, char *pbuf)
{
  int len = 0;

  *mbuf++ = '-';

  for (const struct chan_mode *tab = cmode_tab; tab->letter; ++tab)
    if (tab->mode && (tab->mode & oldmode->mode) && !(tab->mode & mode->mode))
      *mbuf++ = tab->letter;

  if (oldmode->limit && mode->limit == 0)
    *mbuf++ = 'l';

  if (oldmode->key[0] && mode->key[0] == '\0')
  {
    *mbuf++ = 'k';
    len = sprintf(pbuf, "%s ", oldmode->key);
    pbuf += len;
  }

  if (*(mbuf - 1) == '-')
    *(mbuf - 1) = '+';
  else
    *mbuf++ = '+';

  for (const struct chan_mode *tab = cmode_tab; tab->letter; ++tab)
    if (tab->mode && (tab->mode & mode->mode) && !(tab->mode & oldmode->mode))
      *mbuf++ = tab->letter;

  if (mode->limit && oldmode->limit != mode->limit)
  {
    *mbuf++ = 'l';
    len = sprintf(pbuf, "%u ", mode->limit);
    pbuf += len;
  }

  if (mode->key[0] && strcmp(oldmode->key, mode->key))
  {
    *mbuf++ = 'k';
    len = sprintf(pbuf, "%s ", mode->key);
    pbuf += len;
  }

  if (*(mbuf - 1) == '+')
    *(mbuf - 1) = '\0';
  else
    *mbuf = '\0';
}

/* remove_our_modes()
 *
 * inputs	- pointer to channel to remove modes from
 *		- client pointer
 * output	- NONE
 * side effects	- Go through the local members, remove all their
 *		  chanop modes etc., this side lost the TS.
 */
static void
remove_our_modes(struct Channel *chptr, struct Client *source_p)
{
  remove_a_mode(chptr, source_p, CHFL_CHANOP, 'o');
  remove_a_mode(chptr, source_p, CHFL_HALFOP, 'h');
  remove_a_mode(chptr, source_p, CHFL_VOICE, 'v');
}

/* remove_a_mode()
 *
 * inputs	- pointer to channel
 *		- server or client removing the mode
 *		- mask o/h/v mask to be removed
 *		- flag o/h/v to be removed
 * output	- NONE
 * side effects	- remove ONE mode from all members of a channel
 */
static void
remove_a_mode(struct Channel *chptr, struct Client *source_p,
             int mask, char flag)
{
  dlink_node *node = NULL;
  char lmodebuf[MODEBUFLEN];
  char sendbuf[MODEBUFLEN];
  char *sp = sendbuf;
  const char *lpara[MAXMODEPARAMS];
  unsigned int count = 0;
  int l = 0;
  char *mbuf = lmodebuf;

  *mbuf++ = '-';
  *sp = '\0';

  DLINK_FOREACH(node, chptr->members.head)
  {
    struct Membership *member = node->data;

    if ((member->flags & mask) == 0)
      continue;

    member->flags &= ~mask;

    lpara[count++] = member->client_p->name;

    *mbuf++ = flag;

    if (count >= MAXMODEPARAMS)
    {
      for (unsigned int i = 0; i < MAXMODEPARAMS; ++i)
      {
        l = sprintf(sp, " %s", lpara[i]);
        sp += l;
      }

      *mbuf = '\0';
      sendto_channel_local(NULL, chptr, 0, 0, 0, ":%s MODE %s %s%s",
                           (IsHidden(source_p) || ConfigServerHide.hide_servers) ?
                           me.name : source_p->name,
                           chptr->name, lmodebuf, sendbuf);
      mbuf = lmodebuf;
      *mbuf++ = '-';
      count = 0;
      sp = sendbuf;
      *sp = '\0';
    }
  }

  if (count)
  {
    *mbuf = '\0';

    for (unsigned int i = 0; i < count; ++i)
    {
      l = sprintf(sp, " %s", lpara[i]);
      sp += l;
    }

    sendto_channel_local(NULL, chptr, 0, 0, 0, ":%s MODE %s %s%s",
                         (IsHidden(source_p) || ConfigServerHide.hide_servers) ?
                         me.name : source_p->name,
                         chptr->name, lmodebuf, sendbuf);
  }
}

/* remove_ban_list()
 *
 * inputs	- channel, source, list to remove, char of mode
 * outputs	- none
 * side effects	- given ban list is removed, modes are sent to local clients
 */
static void
remove_ban_list(struct Channel *chptr, struct Client *source_p, dlink_list *list, char c)
{
  dlink_node *node, *node_next;
  char modebuf[IRCD_BUFSIZE];
  char parabuf[IRCD_BUFSIZE];
  char *mbuf;
  char *pbuf;
  int count = 0;
  int cur_len, mlen;

  if (dlink_list_length(list) == 0)
    return;

  cur_len = mlen = snprintf(modebuf, sizeof(modebuf), ":%s MODE %s -",
                            source_p->name, chptr->name);
  mbuf = modebuf + mlen;
  pbuf = parabuf;

  DLINK_FOREACH_SAFE(node, node_next, list->head)
  {
    struct Ban *ban = node->data;
    int plen = ban->len + 4;  /* another +b and "!@ " */

    if (count >= MAXMODEPARAMS ||
        (cur_len + 1 /* space between */ + (plen - 1)) > IRCD_BUFSIZE - 2)
    {
      /* NUL-terminate and remove trailing space */
      *mbuf = *(pbuf - 1) = '\0';
      sendto_channel_local(NULL, chptr, 0, 0, 0, "%s %s", modebuf, parabuf);

      cur_len = mlen;
      mbuf = modebuf + mlen;
      pbuf = parabuf;
      count = 0;
    }

    *mbuf++ = c;
    cur_len += plen;
    pbuf += sprintf(pbuf, "%s!%s@%s ", ban->name, ban->user, ban->host);
    ++count;

    remove_ban(ban, list);
  }

  *mbuf = *(pbuf - 1) = '\0';
  sendto_channel_local(NULL, chptr, 0, 0, 0, "%s %s", modebuf, parabuf);
}

static struct Message sjoin_msgtab =
{
  .cmd = "SJOIN",
  .args_min = 5,
  .args_max = MAXPARA,
  .handlers[UNREGISTERED_HANDLER] = m_unregistered,
  .handlers[CLIENT_HANDLER] = m_ignore,
  .handlers[SERVER_HANDLER] = ms_sjoin,
  .handlers[ENCAP_HANDLER] = m_ignore,
  .handlers[OPER_HANDLER] = m_ignore
};

static void
module_init(void)
{
  mod_add_cmd(&sjoin_msgtab);
}

static void
module_exit(void)
{
  mod_del_cmd(&sjoin_msgtab);
}

struct module module_entry =
{
  .version = "$Revision$",
  .modinit = module_init,
  .modexit = module_exit,
  .flags   = MODULE_FLAG_CORE
};
