/*
 *  ircd-hybrid: an advanced, lightweight Internet Relay Chat Daemon (ircd)
 *
 *  Copyright (c) 1997-2016 ircd-hybrid development team
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

/*! \file whowas.c
 * \brief WHOWAS user cache.
 * \version $Id$
 */

#include "stdinc.h"
#include "list.h"
#include "mempool.h"
#include "whowas.h"
#include "client.h"
#include "hash.h"
#include "irc_string.h"
#include "ircd.h"
#include "send.h"
#include "conf.h"


static mp_pool_t *whowas_pool;

static dlink_list whowas_list;  /*! Chain of struct Whowas pointers */
dlink_list WHOWASHASH[HASHSIZE];


/*! \brief Initializes the whowas memory pool
 */
void
whowas_init(void)
{
  whowas_pool = mp_pool_new(sizeof(struct Membership), MP_CHUNK_SIZE_WHOWAS);
}

/*! \brief Unlinks a Whowas struct from its associated lists.
 * \param whowas Pointer to Whowas struct to be unlinked
 */
static struct Whowas *
whowas_unlink(struct Whowas *whowas)
{
  if (!whowas)  /* Can be NULL. */
    return NULL;

  if (whowas->online)
    dlinkDelete(&whowas->cnode, &whowas->online->whowas);

  dlinkDelete(&whowas->tnode, &WHOWASHASH[whowas->hashv]);
  dlinkDelete(&whowas->lnode, &whowas_list);

  return whowas;
}

/*! \brief Unlinks a Whowas struct from its associated lists
 *         and returns memory back to the pooling allocator.
 * \param whowas Pointer to Whowas struct to be unlinked and freed
 */
static void
whowas_free(struct Whowas *whowas)
{
  whowas_unlink(whowas);
  mp_pool_release(whowas);
}

/*! \brief Returns a Whowas struct for further use. Either allocates
 *         a new one, or returns the oldest entry from the whowas_list
 *         if it ran over ConfigGeneral.whowas_history_length
 */
static struct Whowas *
whowas_make(void)
{
  struct Whowas *whowas;

  if (dlink_list_length(&whowas_list) >= ConfigGeneral.whowas_history_length)
    whowas = whowas_unlink(whowas_list.tail->data);  /* Re-use oldest item */
  else
    whowas = mp_pool_get(whowas_pool);

  return whowas;
}

/*! \brief Trims the whowas_list if necessary until there are no
 *         more than ConfigGeneral.whowas_history_length Whowas
 *         struct items.
 */
void
whowas_trim(void)
{
  while (dlink_list_length(&whowas_list) >= ConfigGeneral.whowas_history_length)
  {
    if (!whowas_list.tail->data)
      return;  /* whowas_list is now empty. No more items can be freed. */

    whowas_free(whowas_list.tail->data);
  }
}

/*! \brief Adds the currently defined name of the client to history.
 *         Usually called before changing to a new name (nick).
 *         Client must be a fully registered user.
 * \param client_p pointer to Client struct to add
 * \param online   either 1 if it's a nick change or 0 on client exit
 */
void
whowas_add_history(struct Client *client_p, const int online)
{
  struct Whowas *whowas = whowas_make();

  if (!whowas)  /* Can be NULL. */
    return;

  assert(IsClient(client_p));

  whowas->hashv = strhash(client_p->name);
  whowas->shide = IsHidden(client_p->servptr) != 0;
  whowas->logoff = CurrentTime;

  strlcpy(whowas->account, client_p->account, sizeof(whowas->account));
  strlcpy(whowas->name, client_p->name, sizeof(whowas->name));
  strlcpy(whowas->username, client_p->username, sizeof(whowas->username));
  strlcpy(whowas->hostname, client_p->host, sizeof(whowas->hostname));
  strlcpy(whowas->sockhost, client_p->sockhost, sizeof(whowas->sockhost));
  strlcpy(whowas->realname, client_p->info, sizeof(whowas->realname));
  strlcpy(whowas->servername, client_p->servptr->name, sizeof(whowas->servername));

  if (online)
  {
    whowas->online = client_p;
    dlinkAdd(whowas, &whowas->cnode, &client_p->whowas);
  }
  else
    whowas->online = NULL;

  dlinkAdd(whowas, &whowas->tnode, &WHOWASHASH[whowas->hashv]);
  dlinkAdd(whowas, &whowas->lnode, &whowas_list);
}

/*! \brief This must be called when the client structure is about to
 *         be released. History mechanism keeps pointers to client
 *         structures and it must know when they cease to exist.
 * \param client_p pointer to Client struct
 */
void
whowas_off_history(struct Client *client_p)
{
  while (client_p->whowas.head)
  {
    struct Whowas *whowas = client_p->whowas.head->data;

    whowas->online = NULL;
    dlinkDelete(&whowas->cnode, &client_p->whowas);
  }
}

/*! \brief Returns the current client that was using the given
 *         nickname within the timelimit. Returns NULL, if no
 *         one found.
 * \param name      name of the nick
 * \param timelimit maximum age for a client since log-off
 */
struct Client *
whowas_get_history(const char *name, uintmax_t timelimit)
{
  dlink_node *node = NULL;

  timelimit = CurrentTime - timelimit;

  DLINK_FOREACH(node, WHOWASHASH[strhash(name)].head)
  {
    struct Whowas *whowas = node->data;

    if (whowas->logoff < timelimit)
      continue;
    if (irccmp(name, whowas->name))
      continue;
    return whowas->online;
  }

  return NULL;
}

/*! \brief For debugging. Counts allocated structures stored in whowas_list
 */
void
whowas_count_memory(unsigned int *const count, size_t *const bytes)
{
  (*count) = dlink_list_length(&whowas_list);
  (*bytes) = dlink_list_length(&whowas_list) * sizeof(struct Whowas);
}
