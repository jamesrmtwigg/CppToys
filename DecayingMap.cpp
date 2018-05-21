#include <map>
#include <chrono>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>

using namespace std;
using namespace std::chrono;

template <class K, class V> class DecayingPointer {
	milliseconds ttd;
	K key;
	shared_ptr<V> ptr;

	DecayingPointer(K k, V* v, milliseconds ttd) : key(k), ttd(ttd) {
		ptr = shared_ptr<V>(v);
	}
};

/**
 * A map where an element is removed if it is not accessed within a configurable
 * timeout (default 5s). If this map had the last reference to that object then
 * it is destroyed at the time of removal.
 */
template <class K, class V>
class DecayingMap {
/**
 * TODO
 * Launch Janitor thread which deletes expired elements.
 * How to schedule the janitor thread to wake up when an element decays.
 * How to REschedule the janitor when the next item to decay is refreshed.
 */

private:
	//Future project: a generic (templated) map with two keys. Or an arbitrary number?
	map<K,DecayingPointer<K,V>> dataByKey;
	map<milliseconds, DecayingPointer<K,V>> dataByTtd;
	milliseconds decayTime = milliseconds(5000);
	thread janitor = thread(purgeExpired);
	mutex mtx;
	atomic_flag shutdownFlag = ATOMIC_FLAG_INIT;

	/**
	 * Finds the element of dataByTtd with the given key and ttd, if it exists.
	 * Returns an iterator to that element if it exists, or an iterator to
	 *   dataByTtd.end() if not.
	 */
	typename map<milliseconds,DecayingPointer<K,V>>::iterator findByMillisAndKey(K key, milliseconds ms) {
		auto it = dataByTtd.find(ms);
		if(it == dataByTtd.end()) {
			return it;
		}

		for(; it != dataByTtd.end() && it->ttd == ms; ++it) {
			if(it->key == key) return it;
		}
		return dataByTtd.end();
	}

	milliseconds millisUntil(milliseconds ms) {
		return duration_cast<milliseconds>(ms - system_clock::now().time_since_epoch());
	}

	milliseconds nowPlusDecay() {
		return duration_cast<milliseconds>(system_clock::now().time_since_epoch() + decayTime);
	}

	void purgeExpired() {
		while (true) {
			if(shutdownFlag.test_and_set(memory_order_acquire)){
				//this object is being destructed
				return;
			}
			mtx.lock();
			if(dataByTtd.empty()) {
				mtx.unlock();
				this_thread::sleep_for(decayTime);
				continue;
			}
			//there are some elements in the map
			milliseconds now = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
			auto bound = dataByTtd.upper_bound(now);
			if(bound == dataByTtd.begin()) {
				//no elements have expired yet, sleep until the first expires
				mtx.unlock();
				this_thread::sleep_for(millisUntil(bound->first));
				continue;
			}
			//delete expired elements
			if(bound == dataByTtd.end()) {
				//All elements expired, can simply clear.
				dataByTtd.clear();
				dataByKey.clear();
				mtx.unlock();
				this_thread::sleep_for(decayTime);
				continue;
			}

			//some but not all elements expired
			for(auto it = dataByTtd.begin(); it < bound; ++it) {
				dataByKey.erase(it->second->key);
			}
			dataByTtd.erase(dataByTtd.begin(), bound);
			mtx.unlock();
			this_thread::sleep_for(millisUntil(bound->first));
		}
	}

public:
	DecayingMap(long decayTime = 5000L) {
		this->decayTime = milliseconds(decayTime);
	}

	~DecayingMap() {
		shutdownFlag.test_and_set(memory_order_acq_rel);
		janitor.join();
	}

	/**
	 * Inserts the given key and value into the map.
	 * If there's already an entry with this key it is removed from the map,
	 *   and if that is the last reference to the object it is deleted.
	 * Returns a shared_ptr to the given object.
	 */
	shared_ptr<V> put(K key, V* value) {
		lock_guard<mutex> lock(mtx);
		erase(key);//does nothing if no key
		milliseconds ttd = nowPlusDecay();
		auto elem = DecayingPointer<K,V>(key, value, ttd);
		dataByKey[key] = elem;
		dataByTtd[ttd] = elem;
		//force creation of new reference
		return shared_ptr<V>(dataByKey[key].ptr);
	}

	/**
	 * Erases from this map the element with key 'key', if there is one.
	 * If so destructs the shared_ptr to that object and returns true.
	 * Otherwise does nothing and returns false.
	 */
	bool erase(K key) {
		lock_guard<mutex> lock(mtx);
		auto keyItr = dataByKey.find(key);
		if(keyItr == dataByKey.end()) {
			return false;
		}

		auto ttdItr = findByMillisAndKey(keyItr->key, keyItr->ttd);
		dataByKey.erase(keyItr);
		if(ttdItr != dataByTtd.end()) { //should always be true.
			dataByTtd.erase(ttdItr);
		}
		return true;
	}

	/**
	 * Retrieves an element from the map, refreshing its decay time.
	 * If there is no element in the map with this key returns an empty ptr.
	 */
	shared_ptr<V> get(K key) {
		lock_guard<mutex> lock(mtx);
		auto keyItr = dataByKey.find(key);
		if(keyItr == dataByKey.end()) {
			return shared_ptr<V>();
		}
		auto ttdItr = findByMillisAndKey(key, keyItr->ttd);
		if(ttdItr != dataByTtd.end()) {
			dataByTtd.erase(ttdItr);
		}
		keyItr->ttd = nowPlusDecay();
		dataByTtd[keyItr->ttd] = *keyItr;
		return shared_ptr<V>(keyItr->ptr);
	}
};
