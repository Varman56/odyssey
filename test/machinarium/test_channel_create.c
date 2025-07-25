
#include <machinarium.h>
#include <odyssey_test.h>

static void test_coroutine(void *arg)
{
	(void)arg;
	machine_channel_t *channel;
	channel = machine_channel_create();
	test(channel != NULL);
	machine_channel_free(channel);
}

void machinarium_test_channel_create(void)
{
	machinarium_init();

	int id;
	id = machine_create("test", test_coroutine, NULL);
	test(id != -1);

	int rc;
	rc = machine_wait(id);
	test(rc != -1);

	machinarium_free();
}
