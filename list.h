#include <stddef.h>

#define container_of(x, t, f) ((void *) ((char *) (x) - offsetof(t, f)))
#define list_foreach(p, l) for ((p) = (l)->next; (p) != (l); (p) = (p)->next)

struct list_node {
	struct list_node *prev, *next;
};

static void list_init(struct list_node *head)
{
	head->prev = head->next = head;
}

static void list_link(struct list_node *prev, struct list_node *next)
{
	prev->next = next;
	next->prev = prev;
}

static void list_add(struct list_node *head, struct list_node *node)
{
	list_link(node, head->next);
	list_link(head, node);
}

static void list_add_tail(struct list_node *head, struct list_node *node)
{
	list_link(head->prev, node);
	list_link(node, head);
}

static void list_del(struct list_node *node)
{
	list_link(node->prev, node->next);
}
