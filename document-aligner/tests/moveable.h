#pragma once
#include <iostream>
#include <cassert>

#ifndef MOVEABLE_DEBUG
#define MOVEABLE_DEBUG false
#endif

namespace tests {

/**
 * Test class to track copy & move behaviour
 */
class Moveable {
public:	
	Moveable() : id_(-1), copy_(0), alive_(true) {

	}

	Moveable(int id) : id_(id), copy_(0), alive_(true) {
		if (MOVEABLE_DEBUG) std::cerr << "Moveable " << id_ << " copy " << copy_ << " is created" << std::endl;
	}

	~Moveable() {
		if (MOVEABLE_DEBUG) std::cerr << "Moveable " << id_ << " copy " << copy_ << " is freed" << std::endl;
		assert(alive_);
		alive_ = false;
	}

	Moveable(Moveable const &other) : id_(other.id_), copy_(other.copy_ + 1), alive_(true) {
		if (MOVEABLE_DEBUG) std::cerr << "Moveable " << id_ << " copy " << copy_ << " is copy-constructed" << std::endl;
		assert(other.alive_);
	}

	Moveable &operator=(Moveable const &other) {
		if (MOVEABLE_DEBUG) std::cerr << "Moveable " << other.id_ << " copy " << (other.copy_+1) << " is copy-assigned over " << id_ << "(" << copy_ << ")" << std::endl;
		assert(other.alive_);
		id_ = other.id_;
		copy_ = other.copy_ + 1;
		alive_ = other.alive_;
		return *this;
	} 

	Moveable(Moveable &&other) : id_(other.id_), copy_(other.copy_), alive_(true) {
		if (MOVEABLE_DEBUG) std::cerr << "Moveable " << id_ << " copy " << copy_ << " is move-constructed" << std::endl;
		assert(other.alive_);
		other.alive_ = false;
	}

	Moveable &operator=(Moveable &&other) {
		if (MOVEABLE_DEBUG) std::cerr << "Moveable " << other.id_ << " copy " << (other.copy_+1) << " is move-assigned over " << id_ << "(" << copy_ << ")" << std::endl;
		assert(other.alive_);
		id_ = other.id_;
		copy_ = other.copy_;
		alive_ = other.alive_;
		other.alive_ = false;
		return *this;
	}

	bool isAlive() const {
		if (MOVEABLE_DEBUG) std::cerr << "Is Moveable " << id_ << " copy " << copy_ << " alive? " << (alive_ ? "yes" : "nope") << std::endl;
		return alive_;
	}
private:
	int id_;
	int copy_;
	bool alive_;
};

} // namespace
