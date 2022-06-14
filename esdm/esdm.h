/*
 * Copyright (C) 2022, Stephan Mueller <smueller@chronox.de>
 *
 * License: see LICENSE file in root directory
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#ifndef _ESDM_H
#define _ESDM_H

#include <stdint.h>
#include <sys/types.h>

/**
 * esdm_init() - initialize the ESDM library
 *
 * @return: 0 on success, < 0 on error
 */
int esdm_init(void);

/**
 * esdm_fini - finalize the ESDM library and release all resources
 */
void esdm_fini(void);

/**
 * @brief esdm_get_random_bytes() - Provider of cryptographic strong random
 * numbers without a guarantee of the ESDM being properly seeded
 *
 * It provides access to the full functionality of ESDM including the
 * switchable DRNG support, that may support other DRNGs such as the SP800-90A
 * DRBG.
 *
 * @buf: buffer to store the random bytes
 * @nbytes: size of the buffer
 */
ssize_t esdm_get_random_bytes(uint8_t *buf, size_t nbytes);

/**
 * @brief esdm_get_random_bytes_full() - Provider of cryptographic strong
 * random numbers from a fully initialized ESDM.
 *
 * This function will always return random numbers from a fully seeded and
 * fully initialized ESDM.
 *
 * It provides access to the full functionality of ESDM including the
 * switchable DRNG support, that may support other DRNGs such as the SP800-90A
 * DRBG.
 *
 * @buf: buffer to store the random bytes
 * @nbytes: size of the buffer
 *
 * @return: positive number indicates amount of generated bytes, < 0 on error
 */
ssize_t esdm_get_random_bytes_full(uint8_t *buf, size_t nbytes);

/**
 * @brief esdm_get_random_bytes_min() - Provider of cryptographic strong
 * random numbers from at least a minimally seeded ESDM, which is not
 * necessarily fully initialized yet (e.g. SP800-90C oversampling applied in
 * FIPS mode is not applied yet).
 *
 * It provides access to the full functionality of ESDM including the
 * switchable DRNG support, that may support other DRNGs such as the SP800-90A
 * DRBG.
 *
 * @buf: buffer to store the random bytes
 * @nbytes: size of the buffer
 *
 * @return: positive number indicates amount of generated bytes, < 0 on error
 */
ssize_t esdm_get_random_bytes_min(uint8_t *buf, size_t nbytes);

/**
 * @brief esdm_get_random_bytes_pr() - Provider of cryptographic strong
 * random numbers from a fully initialized ESDM and requiring a reseed
 * from the entropy sources before.
 *
 * This function will always return random numbers from a fully seeded and
 * fully initialized ESDM.
 *
 * This call only returns no more data than entropy was pulled from the
 * entropy sources. Thus, it is likely that this call returns less data
 * than requested by the caller. Also, the caller MUST be prepared that this
 * call returns 0 bytes, i.e. it did not generate data.
 *
 * @buf: buffer to store the random bytes
 * @nbytes: size of the buffer
 *
 * @return: positive number indicates amount of generated bytes, < 0 on error
 */
ssize_t esdm_get_random_bytes_pr(uint8_t *buf, size_t nbytes);

/**
 * @brief esdm_status() - Get status information on ESDM
 *
 * @param buf [out] Buffer to be filled with status information
 * @param buflen [in] Length of buffer
 */
void esdm_status(char *buf, size_t buflen);

/**
 * @brief esdm_version() - Get ESDM version information
 *
 * @param buf [out] Buffer to be filled with status information
 * @param buflen [in] Length of buffer
 */
void esdm_version(char *buf, size_t buflen);

/**
 * @brief Insert entropy into the auxiliary pool
 *
 * External entities are allowed to insert entropy into the auxiliary pool.
 * The auxiliary pool therefore is a separate entropy source.
 *
 * NOTE: When wanting to operate the ESDM SP800-90C compliant, make sure that
 * you only insert data from an SP800-90B entropy source where entropy_bits
 * is set to a value > 0. When the value is 0, the data does not need to be
 * provided by an SP800-90B entropy source.
 *
 * @param inbuf [in] Buffer with the data to be inserted into the aux pool.
 * @param inbuflen [in] Size of the buffer.
 * @param entropy_bits [in] Amount of bits to be credited for the inserted
 *			    data.
 *
 * @return: 0 on success, < 0 on error
 */
int esdm_pool_insert_aux(const uint8_t *inbuf, size_t inbuflen,
			 uint32_t entropy_bits);

/**
 * @brief Obtain the available entropy in all ESDM entropy pools in bits
 *
 * @return available entropy in bits
 */
uint32_t esdm_avail_entropy(void);

/**
 * @brief Obtain the available entropy in the aux ESDM entropy pool in bits
 *
 * @return available entropy in bits
 */
uint32_t esdm_avail_entropy_aux(void);

/**
 * @brief Obtain the poolsize of the aux ESDM entropy pool in bits
 *
 * @return poolsize in bits
 */
uint32_t esdm_avail_poolsize_aux(void);

/**
 * @brief Obtain the available entropy of the auxiliary pool in bits
 *
 * @return available entropy in bits
 */
uint32_t esdm_get_aux_ent(void);

/**
 * @brief Obtain the size of the message digest of the conditioner used by the
 *	  ESDM
 *
 * @return message digest size in bits
 */
uint32_t esdm_get_digestsize(void);

/**
 * @brief Set the entropy level of the auxiliary pool
 *
 * @param entropy_bits [in] Entropy rate in bits
 */
void esdm_pool_set_entropy(uint32_t entropy_bits);

/**
 * @brief Force a reseed of all DRNGs in the ESDM
 *
 * The call only sets a flag for the reseed, the actual reseed is performed
 * the next time the DRNG is requested to deliver random data.
 */
void esdm_drng_force_reseed(void);

/**
 * @brief Indicator whether the ESDM is operational
 *
 * The ESDM is operational if at least one DRNG is fully seeded with the amount
 * of entropy equal to the DRNG security strength (regular mode) or with the
 * amount of entropy stipulated by SP800-90C (FIPS mode).
 *
 * @return 1 if operational, 0 if not operational
 */
int esdm_state_operational(void);

/**
 * @brief Indicator whether ESDM operates SP800-90C compliant
 *
 * @return 1 if SP800-90C compliant, 0 if not SP800-90C compliant
 */
int esdm_sp80090c_compliant(void);

/**
 * @brief Indicator whether at least one DRNG is fully seeded
 *
 * @return 1 if fully seeded, 0 if not fully seeded
 */
int esdm_state_fully_seeded(void);

/**
 * @brief Get write wakeup in bits
 *
 * @return write wakeup in bits
 */
uint32_t esdm_get_write_wakeup_bits(void);

/**
 * @brief Set write wakeup in bits
 */
void esdm_set_write_wakeup_bits(uint32_t val);

/**
 * @brief Get maximum reseed interval in seconds
 *
 * @return maximum reseed interval in seconds
 */
uint32_t esdm_get_reseed_max_time(void);

/**
 * @brief Set maximum reseed interval in seconds
 */
void esdm_set_reseed_max_time(uint32_t seconds);

#endif /* _ESDM_H */
