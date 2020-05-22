#define BOOST_TEST_MODULE broadcast_queue
#include <atomic>
#include <vector>
#include <thread>
#include <iostream>
#include <numeric>
#include <boost/test/unit_test.hpp>
#include "../src/broadcast_queue.h"

#define MOVEABLE_DEBUG false
#include "moveable.h"

using namespace std;
using namespace bitextor;
using tests::Moveable;


/**
 * When you submit messages to multiple listeners, all messages are delivered to
 * all listeners.
 */
BOOST_AUTO_TEST_CASE(test_every_message_delivered)
{
	const size_t NUM_THREADS = 64;
	const size_t NUM_MESSAGES = 300; // Bit more than 2 pages

	array<atomic<int>,10> totals;
	array<int,10> expected_totals;
	
	// Initialize my arrays (I would have used vectors and RAII but atomics are
	// tricky since they can't be copied nor moved.)
	for (size_t i = 0; i < 10; ++i) {
		totals[i] = 0;
		expected_totals[i] = NUM_THREADS * ((NUM_MESSAGES / 10) + (NUM_MESSAGES % 10 > i ? 1 : 0));
	}

	BroadcastQueue<int, 128> messages;

	vector<thread> threads;

	for (size_t i = 0; i < NUM_THREADS; ++i)
		threads.emplace_back([&totals](BroadcastQueue<int,128>::listener listener) {
			int message;
			while (listener.pop(message) >= 0)
				totals[message]++;
		}, messages.listen());

	// Start producing messages
	for (size_t i = 0; i < NUM_MESSAGES; ++i)
		messages.push(i % 10);

	// Tell workers to stop
	for (size_t i = 0; i < NUM_THREADS; ++i)
		messages.push(-1);

	for (auto &thread : threads)
		thread.join();

	BOOST_TEST(totals == expected_totals, boost::test_tools::per_element());
}


/**
 * When you submit messages to multiple listeners, each listener receives all
 * messages once.
 */
BOOST_AUTO_TEST_CASE(test_every_message_delivered_once)
{
	const size_t NUM_THREADS = 4;
	const size_t NUM_MESSAGES = 9001; // Bit more than 2 pages

	array<int,10> expected_counters;
	array<atomic<int>,10> totals;
	
	for (size_t i = 0; i < 10; ++i) {
		totals[i] = 0;
		expected_counters[i] = (NUM_MESSAGES / 10) + (NUM_MESSAGES % 10 > i ? 1 : 0);
	}

	BroadcastQueue<int> messages;

	vector<thread> threads;

	for (size_t i = 0; i < NUM_THREADS; ++i)
		threads.emplace_back([&expected_counters](BroadcastQueue<int>::listener listener) {
			array<int,10> counters{0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

			int message;
			while (listener.pop(message) >= 0)
				counters[message]++;

			BOOST_TEST(counters == expected_counters, boost::test_tools::per_element());
		}, messages.listen());

	// Start producing messages
	for (size_t i = 0; i < NUM_MESSAGES; ++i)
		messages.push(i % 10);

	// Tell workers to stop
	for (size_t i = 0; i < NUM_THREADS; ++i)
		messages.push(-1);

	for (auto &thread : threads)
		thread.join();
}


/**
 * When you start listening after a couple of messages, you will only receive
 * messages send after you started listening.
 */
BOOST_AUTO_TEST_CASE(test_listen_after_first_message)
{
	BroadcastQueue<int> messages;

	messages.push(1);
	messages.push(2);

	auto late_listener(messages.listen());

	messages.push(3);
	messages.push(4);

	BOOST_TEST(late_listener.pop() == 3);
	BOOST_TEST(late_listener.pop() == 4);
}

/**
 * When the value comes out of the queue, it should still be alive.
 */
BOOST_AUTO_TEST_CASE(test_proper_copy_behaviour)
{
	BroadcastQueue<Moveable, 4> messages;

	auto listener(messages.listen());

	Moveable msg1(1);
	messages.push(msg1);

	BOOST_TEST(msg1.isAlive());
	
	BOOST_TEST(listener.pop().isAlive());
}

/**
 * When calling pop on an uninitialized listener, you should get a logic_error
 */
BOOST_AUTO_TEST_CASE(test_queue_exception)
{
	BroadcastQueue<Moveable,4>::listener listener;
	BOOST_CHECK_THROW(listener.pop(), logic_error);
}

/**
 * When you deallocate the queue before all listeners end, the listeners should
 * still function.
 */
BOOST_AUTO_TEST_CASE(test_queue_lifetime)
{
	BroadcastQueue<Moveable,4>::listener listener;
	
	{
		BroadcastQueue<Moveable, 4> messages;
		listener = messages.listen();

		Moveable msg1(1);
		messages.push(msg1);
	}

	BOOST_TEST(listener.pop().isAlive());
}
