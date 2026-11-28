#include <atomic>

#include <frg/manual_box.hpp>
#include <cralgo/aes.hpp>
#include <cralgo/sha2_32.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch-generic/timer.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/random.hpp>
#include <thor-internal/debug.hpp>

namespace thor {

namespace {

struct Fortuna {
	// Key size of the block cipher in bytes.
	// This must also equal the hash function's digest size.
	static constexpr int keySize = 32;

	// Block size of the block cipher in bytes.
	// Must divide the blockSize (due to the key-regneration in generate()).
	static constexpr int blockSize = 16;

	// Must be a power of 2 (due to the seqNum -> pool ID division in injectEntropy).
	static constexpr int numPools = 32;

	Fortuna() {
		memset(keyBytes_, 0, keySize);
		memset(ctrBlock_, 0, blockSize);
		ctrBlock_[0] = 1;

		for(int k = 0; k < numPools; ++k) {
			auto pool = &pools_[k];
			cralgo::sha256_clear(&pool->entropyHash);
		}
	}

	void injectEntropy(uint8_t entropySource, unsigned int seqNum,
			const void *buffer, size_t size) {
		assert(size <= 32 && "Entropy sources should hash their data instead of"
				" large buffers into injectEntropy()");

		uint8_t prefix[2] = {entropySource, static_cast<uint8_t>(size)};

		auto k = seqNum & (numPools - 1);
		auto pool = &pools_[k];
		{
			auto poolLock = frg::guard(&pool->poolMutex);

			cralgo::sha256_update(&pool->entropyHash, prefix, 2);
			cralgo::sha256_update(&pool->entropyHash, (const uint8_t *)buffer, size);
			if(!k) {
				// TODO: for 32-bit size_t, this could potentially overflow.
				//       For now, this should not be an issue though.
				injectedIntoPoolZero_.fetch_add(2 + size, std::memory_order_release);
			}
		}
	}

	void forceReseed(const void *seed, size_t size) {
		cralgo::sha2_32_secrets keyHash;
		uint8_t tempDigest[keySize];

		auto generatorLock = frg::guard(&generatorMutex_);

		// First, hash in the current block cipher key.
		cralgo::sha256_clear(&keyHash);
		cralgo::sha256_update(&keyHash, keyBytes_, keySize);

		// Secondly, hash in entropy.
		cralgo::sha256_update(&keyHash, (const uint8_t *)seed, size);

		// Update the block cipher key by applying SHA256d.
		cralgo::sha256_finalize(&keyHash, tempDigest);
		cralgo::sha256_clear(&keyHash);
		cralgo::sha256_update(&keyHash, tempDigest, keySize);
		cralgo::sha256_finalize(&keyHash, keyBytes_);
	}

	size_t generate(void *buffer, size_t size) {
		// If we assume that each byte injected into pool zero contains 2 bits of entropy,
		// we need 64 bytes until we reach 128 bits of entropy.
		const size_t entropyThreshold = 64;

		auto generatorLock = frg::guard(&generatorMutex_);

		if(injectedIntoPoolZero_.load(std::memory_order_acquire) >= entropyThreshold) {
			cralgo::sha2_32_secrets keyHash;
			cralgo::sha2_32_secrets localHash;
			uint8_t tempDigest[keySize];

			// First, hash in the current block cipher key.
			cralgo::sha256_clear(&keyHash);
			cralgo::sha256_update(&keyHash, keyBytes_, keySize);

			// Secondly, hash in entropy.
			for(int k = 0; k < numPools; ++k) {
				if(reseedNumber_ & ((uint32_t{1} << k) - 1))
					break;
				auto pool = &pools_[k];
				{
					auto poolLock = frg::guard(&pool->poolMutex);

					cralgo::sha256_finalize(&pool->entropyHash, tempDigest);
					cralgo::sha256_clear(&pool->entropyHash);
				}

				// Apply SHA256d.
				cralgo::sha256_clear(&localHash);
				cralgo::sha256_update(&localHash, tempDigest, keySize);
				cralgo::sha256_finalize(&localHash, tempDigest);

				// Add the pool's hash to the block cipher hash.
				cralgo::sha256_update(&keyHash, tempDigest, keySize);
			}

			// Update the block cipher key by applying SHA256d.
			cralgo::sha256_finalize(&keyHash, tempDigest);
			cralgo::sha256_clear(&keyHash);
			cralgo::sha256_update(&keyHash, tempDigest, keySize);
			cralgo::sha256_finalize(&keyHash, keyBytes_);

			// Note: since we clear pool zero (and drop the lock) in the loop above but only reset
			//       the counter here, it can happen that there was more entropy injected into
			//       pool zero in the meantime. In that case, however, we will only underestimate
			//       the true amount of entropy in the pool.
			++reseedNumber_;
			injectedIntoPoolZero_.store(0, std::memory_order_relaxed);
		}

		cralgo::aes_secret_key ek, dk;
		cralgo::aes256_key_schedule(keyBytes_, &ek, &dk);

		auto generateBlock = [&] (uint8_t *block) {
			cralgo::aes256_encrypt(ctrBlock_, block, 1, &ek);

			// Increment the counter.
			for(int i = 0; i < blockSize; ++i) {
				if(++ctrBlock_[i])
					break;
			}
		};

		auto p = reinterpret_cast<char *>(buffer);
		size_t progress = 0;
		while(progress < size) {
			if(progress >= (1 << 20))
				break;
			size_t chunk = std::min(size - progress, size_t{blockSize});
			uint8_t block[blockSize];
			generateBlock(block);
			memcpy(p + progress, block, chunk);
			progress += chunk;
		}

		// Regenerate the block cipher key.
		for(int i = 0; i < keySize / blockSize; ++i) {
			uint8_t block[blockSize];
			generateBlock(block);
			memcpy(keyBytes_ + blockSize * i, block, blockSize);
		}

		return progress;
	}

private:
	struct Pool {
		IrqSpinlock poolMutex;
		cralgo::sha2_32_secrets entropyHash;
	};

	IrqSpinlock generatorMutex_;

	// These two fields form the generator state.
	// They are protected by generatorMutex_;
	uint8_t keyBytes_[keySize];
	uint8_t ctrBlock_[blockSize];

	// Number of reseeds.
	// This is protected by generatorMutex_;
	uint32_t reseedNumber_ = 1;

	// The remaining fields form the entropy accumulator.
	Pool pools_[numPools];
	std::atomic<size_t> injectedIntoPoolZero_{0};
};

frg::manual_box<Fortuna> csprng;

} // anonymous namespace

void initializeRandom() {
	csprng.initialize();

	uint8_t seed[32]; // 256 bits of entropy should be enough.
	if(auto e = getEntropyFromCpu(seed, 32); e == Error::success) {
		csprng->forceReseed(seed, 32);
		return;
	}else if(e == Error::noHardwareSupport) {
		urgentLogger() << "thor: CPU-based hardware PRNG not available" << frg::endlog;
	}else{
		assert(e == Error::hardwareBroken);
		urgentLogger() << "thor: CPU-based hardware PRNG is broken" << frg::endlog;
	}

	// TODO: we can do something *much* better here, this case is highly insecure!
	//       Use jitter-based entropy (e.g., HAVEGE) instead.
	urgentLogger() << "thor: Falling back to entropy from CPU clock" << frg::endlog;
	uint64_t tsc = getRawTimestampCounter();
	csprng->forceReseed(&tsc, sizeof(uint64_t));
}

void injectEntropy(unsigned int entropySource, unsigned int seqNum, void *buffer, size_t size) {
	csprng->injectEntropy(entropySource, seqNum, buffer, size);
}

size_t generateRandomBytes(void *buffer, size_t size) {
	return csprng->generate(buffer, size);
}

} // namespace thor
