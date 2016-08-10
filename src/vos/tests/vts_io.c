/**
 * (C) Copyright 2016 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
/**
 * This file is part of vos/tests/
 *
 * vos/tests/vts_io.c
 *
 * Author: Vishwanath Venkatesan <vishwanath.venaktesan@intel.com>
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <time.h>
#include <vts_common.h>

#include <daos/common.h>
#include <daos_srv/vos.h>
#include <vos_obj.h>
#include <vos_internal.h>
#include <vos_hhash.h>

#define UPDATE_DKEY_SIZE	32
#define UPDATE_DKEY		"test_update_dkey"
#define UPDATE_AKEY_SIZE	32
#define UPDATE_AKEY		"test_update akey"
#define	UPDATE_BUF_SIZE		64
#define UPDATE_REC_SIZE		16
#define UPDATE_CSUM_SIZE	32
#define VTS_IO_OIDS		1
#define VTS_IO_KEYS		100000

enum {
	TF_IT_ANCHOR		= (1 << 0),
	TF_ZERO_COPY		= (1 << 1),
	TF_OVERWRITE		= (1 << 2),
	TF_PUNCH		= (1 << 3),
	TF_REC_EXT		= (1 << 4),
};

struct io_test_args {
	struct vos_test_ctx	ctx;
	daos_unit_oid_t		oid;
	/* Optional addn container create params */
	uuid_t			addn_co_uuid;
	daos_handle_t		addn_co;
	int			co_create_step;
	/****************************************/
	unsigned long		ta_flags;
};

int		kc;
unsigned int	g_epoch;
/* To verify during enumeration */
unsigned int	total_keys;
unsigned int	total_oids;
daos_epoch_t	max_epoch = 1;

/**
 * Stores the last key and can be used for
 * punching or overwrite
 */
char		last_dkey[UPDATE_DKEY_SIZE];
char		last_akey[UPDATE_AKEY_SIZE];

daos_epoch_t
gen_rand_epoch(void)
{
	max_epoch += rand() % 100;
	return max_epoch;
}

void
gen_rand_key(char *rkey, char *key, int ksize)
{
	int n;

	memset(rkey, 0, ksize);
	n = snprintf(rkey, ksize, key);
	snprintf(rkey+n, ksize-n, ".%d", kc++);
}

static int
setup(void **state)
{
	struct io_test_args	*arg;
	int			rc = 0;

	arg = malloc(sizeof(struct io_test_args));
	assert_ptr_not_equal(arg, NULL);

	kc	= 0;
	g_epoch = 0;
	total_keys = 0;
	total_oids = 0;
	srand(10);

	rc = vts_ctx_init(&arg->ctx, VPOOL_1G);
	assert_int_equal(rc, 0);

	vts_io_set_oid(&arg->oid);
	total_oids++;
	*state = arg;

	return 0;
}

static int
teardown(void **state)
{
	struct io_test_args	*arg = *state;

	vts_ctx_fini(&arg->ctx);
	free(arg);

	return 0;
}

static int
io_recx_iterate(vos_iter_param_t *param, daos_akey_t *akey, int akey_id,
		bool print_ent)
{
	daos_handle_t	ih;
	int		nr = 0;
	int		rc;

	param->ip_akey = *akey;
	rc = vos_iter_prepare(VOS_ITER_RECX, param, &ih);
	if (rc != 0) {
		print_error("Failed to create recx iterator: %d\n", rc);
		goto out;
	}

	rc = vos_iter_probe(ih, NULL);
	if (rc != 0 && rc != -DER_NONEXIST) {
		print_error("Failed to set iterator cursor: %d\n", rc);
		goto out;
	}

	while (rc == 0) {
		vos_iter_entry_t  ent;

		rc = vos_iter_fetch(ih, &ent, NULL);
		if (rc != 0) {
			print_error("Failed to fetch recx: %d\n", rc);
			goto out;
		}

		nr++;
		if (print_ent) {
			if (nr == 1) {
				D_PRINT("akey[%d]: %s\n", akey_id,
					(char *)param->ip_akey.iov_buf);
			}

			D_PRINT("\trecx %u : %s\n",
				(unsigned int)ent.ie_recx.rx_idx,
				ent.ie_iov.iov_len == 0 ?
				"[NULL]" : (char *)ent.ie_iov.iov_buf);
		}

		rc = vos_iter_next(ih);
		if (rc != 0 && rc != -DER_NONEXIST) {
			print_error("Failed to move cursor: %d\n", rc);
			goto out;
		}
	}
	rc = 0;
out:
	vos_iter_finish(ih);
	return rc;
}

static int
io_akey_iterate(vos_iter_param_t *param, daos_dkey_t *dkey, int dkey_id,
		bool print_ent)
{
	daos_handle_t	ih;
	int		nr = 0;
	int		rc;

	param->ip_dkey = *dkey;
	rc = vos_iter_prepare(VOS_ITER_AKEY, param, &ih);
	if (rc != 0) {
		print_error("Failed to create akey iterator: %d\n", rc);
		goto out;
	}

	rc = vos_iter_probe(ih, NULL);
	if (rc != 0 && rc != -DER_NONEXIST) {
		print_error("Failed to set iterator cursor: %d\n", rc);
		goto out;
	}

	while (rc == 0) {
		vos_iter_entry_t  ent;

		rc = vos_iter_fetch(ih, &ent, NULL);
		if (rc != 0) {
			print_error("Failed to fetch akey: %d\n", rc);
			goto out;
		}

		if (print_ent && nr == 0) {
			D_PRINT("dkey[%d]: %s\n", dkey_id,
				(char *)param->ip_dkey.iov_buf);
		}

		rc = io_recx_iterate(param, &ent.ie_key, nr, print_ent);

		nr++;
		rc = vos_iter_next(ih);
		if (rc != 0 && rc != -DER_NONEXIST) {
			print_error("Failed to move cursor: %d\n", rc);
			goto out;
		}
	}
	rc = 0;
out:
	vos_iter_finish(ih);
	return rc;
}

static int
io_obj_iter_test(struct io_test_args *arg)
{
	vos_iter_param_t	param;
	daos_handle_t		ih;
	int			nr = 0;
	int			rc;

	memset(&param, 0, sizeof(param));
	param.ip_hdl	= arg->ctx.tc_co_hdl;
	param.ip_oid	= arg->oid;
	param.ip_epr.epr_lo = max_epoch + 10;

	rc = vos_iter_prepare(VOS_ITER_DKEY, &param, &ih);
	if (rc != 0) {
		print_error("Failed to prepare d-key iterator\n");
		return rc;
	}

	rc = vos_iter_probe(ih, NULL);
	if (rc != 0) {
		print_error("Failed to set iterator cursor: %d\n",
			    rc);
		goto out;
	}

	while (1) {
		vos_iter_entry_t  ent;
		daos_hash_out_t	  anchor;

		rc = vos_iter_fetch(ih, &ent, NULL);
		if (rc == -DER_NONEXIST) {
			print_message("Finishing d-key iteration\n");
			break;
		}

		if (rc != 0) {
			print_error("Failed to fetch dkey: %d\n", rc);
			goto out;
		}

		rc = io_akey_iterate(&param, &ent.ie_key, nr,
				     VTS_IO_KEYS <= 10);
		if (rc != 0)
			goto out;

		nr++;
		rc = vos_iter_next(ih);
		if (rc == -DER_NONEXIST)
			break;

		if (rc != 0) {
			print_error("Failed to move cursor: %d\n", rc);
			goto out;
		}

		if (!(arg->ta_flags & TF_IT_ANCHOR))
			continue;

		rc = vos_iter_fetch(ih, &ent, &anchor);
		if (rc != 0) {
			assert_true(rc != -DER_NONEXIST);
			print_error("Failed to fetch anchor: %d\n",
				    rc);
			goto out;
		}

		rc = vos_iter_probe(ih, &anchor);
		if (rc != 0) {
			assert_true(rc != -DER_NONEXIST);
			print_error("Failed to probe anchor: %d\n",
				    rc);
			goto out;
		}
	}
 out:
	/**
	 * Check if enumerated keys is equal to the number of
	 * keys updated
	 */
	print_message("Enumerated: %d, total_keys: %d\n", nr, total_keys);
	assert_int_equal(nr, total_keys);
	vos_iter_finish(ih);
	return rc;
}

static int
io_test_obj_update(struct io_test_args *arg, int epoch, daos_key_t *dkey,
		   daos_vec_iod_t *vio, daos_sg_list_t *sgl)
{
	daos_sg_list_t	*vec_sgl;
	daos_iov_t	*vec_iov;
	daos_iov_t	*srv_iov;
	daos_handle_t	 ioh;
	int		 rc;

	if (!(arg->ta_flags & TF_ZERO_COPY)) {
		rc = vos_obj_update(arg->ctx.tc_co_hdl,
				    arg->oid, epoch, dkey, 1, vio,
				    sgl, NULL);
		if (rc != 0)
			print_error("Failed to update: %d\n", rc);
		return rc;
	}

	rc = vos_obj_zc_update_begin(arg->ctx.tc_co_hdl,
				     arg->oid, epoch, dkey, 1, vio,
				     &ioh, NULL);
	if (rc != 0) {
		print_error("Failed to prepare ZC update: %d\n", rc);
		return rc;
	}

	srv_iov = &sgl->sg_iovs[0];

	vos_obj_zc_vec2sgl(ioh, 0, &vec_sgl);
	assert_int_equal(vec_sgl->sg_nr.num, 1);
	vec_iov = &vec_sgl->sg_iovs[0];

	assert_true(srv_iov->iov_len == vec_iov->iov_len);
	memcpy(vec_iov->iov_buf, srv_iov->iov_buf, srv_iov->iov_len);

	rc = vos_obj_zc_update_end(ioh, dkey, 1, vio, 0, NULL);
	if (rc != 0)
		print_error("Failed to submit ZC update: %d\n", rc);

	return rc;
}

static int
io_test_obj_fetch(struct io_test_args *arg, int epoch, daos_key_t *dkey,
		  daos_vec_iod_t *vio, daos_sg_list_t *sgl)
{
	daos_sg_list_t	*vec_sgl;
	daos_iov_t	*vec_iov;
	daos_iov_t	*dst_iov;
	daos_handle_t	 ioh;
	int		 rc;

	if (!(arg->ta_flags & TF_ZERO_COPY)) {
		rc = vos_obj_fetch(arg->ctx.tc_co_hdl,
				   arg->oid, epoch, dkey, 1, vio,
				   sgl, NULL);
		if (rc != 0)
			print_error("Failed to fetch: %d\n", rc);

		return rc;
	}

	rc = vos_obj_zc_fetch_begin(arg->ctx.tc_co_hdl,
				    arg->oid, epoch, dkey, 1, vio,
				    &ioh, NULL);
	if (rc != 0) {
		print_error("Failed to prepare ZC update: %d\n", rc);
		return rc;
	}

	dst_iov = &sgl->sg_iovs[0];

	vos_obj_zc_vec2sgl(ioh, 0, &vec_sgl);
	assert_true(vec_sgl->sg_nr.num == 1);
	vec_iov = &vec_sgl->sg_iovs[0];

	assert_true(dst_iov->iov_buf_len >= vec_iov->iov_len);
	memcpy(dst_iov->iov_buf, vec_iov->iov_buf, vec_iov->iov_len);
	dst_iov->iov_len = vec_iov->iov_len;

	rc = vos_obj_zc_fetch_end(ioh, dkey, 1, vio, 0, NULL);
	if (rc != 0)
		print_error("Failed to submit ZC update: %d\n", rc);

	return rc;
}

static int
io_update_and_fetch_dkey(struct io_test_args *arg, daos_epoch_t update_epoch,
			 daos_epoch_t fetch_epoch)
{

	int			rc = 0;
	daos_iov_t		val_iov;
	daos_key_t		dkey;
	daos_key_t		akey;
	daos_recx_t		rex;
	char			dkey_buf[UPDATE_DKEY_SIZE];
	char			akey_buf[UPDATE_DKEY_SIZE];
	char			update_buf[UPDATE_BUF_SIZE];
	char			fetch_buf[UPDATE_BUF_SIZE];
	daos_vec_iod_t		vio;
	daos_sg_list_t		sgl;

	memset(&vio, 0, sizeof(vio));
	memset(&rex, 0, sizeof(rex));
	memset(&sgl, 0, sizeof(sgl));

	if (!(arg->ta_flags & TF_PUNCH)) {
		if (arg->ta_flags & TF_OVERWRITE) {
			memcpy(dkey_buf, last_dkey, UPDATE_DKEY_SIZE);
			memcpy(akey_buf, last_akey, UPDATE_DKEY_SIZE);
		} else {
			gen_rand_key(&dkey_buf[0], UPDATE_DKEY,
				     UPDATE_DKEY_SIZE);
			memcpy(last_dkey, dkey_buf, UPDATE_DKEY_SIZE);

			gen_rand_key(&akey_buf[0], UPDATE_AKEY,
				     UPDATE_DKEY_SIZE);
			memcpy(last_akey, akey_buf, UPDATE_AKEY_SIZE);
		}

		daos_iov_set(&dkey, &dkey_buf[0], strlen(dkey_buf));
		daos_iov_set(&akey, &akey_buf[0], strlen(akey_buf));

		memset(update_buf, (rand() % 94) + 33, UPDATE_BUF_SIZE);
		daos_iov_set(&val_iov, &update_buf[0], UPDATE_BUF_SIZE);
		rex.rx_rsize	= val_iov.iov_len;
	} else {
		daos_iov_set(&dkey, &last_dkey[0], UPDATE_DKEY_SIZE);
		daos_iov_set(&akey, &last_akey[0], UPDATE_AKEY_SIZE);

		memset(update_buf, 0, UPDATE_BUF_SIZE);
		daos_iov_set(&val_iov, &update_buf[0], UPDATE_BUF_SIZE);
		rex.rx_rsize	= 0;
	}

	sgl.sg_nr.num = 1;
	sgl.sg_iovs = &val_iov;

	rex.rx_nr	= 1;
	rex.rx_idx	= daos_hash_string_u32(dkey_buf, dkey.iov_len);
	rex.rx_idx	%= 1000000;

	vio.vd_name	= akey;
	vio.vd_recxs	= &rex;
	vio.vd_nr	= 1;

	rc = io_test_obj_update(arg, update_epoch, &dkey, &vio, &sgl);
	if (rc)
		goto exit;

	if (!(arg->ta_flags & TF_OVERWRITE))
		total_keys++;

	memset(fetch_buf, 0, UPDATE_BUF_SIZE);
	daos_iov_set(&val_iov, &fetch_buf[0], UPDATE_BUF_SIZE);

	rex.rx_rsize = DAOS_REC_ANY;
	rc = io_test_obj_fetch(arg, fetch_epoch, &dkey, &vio, &sgl);
	if (rc)
		goto exit;
	assert_memory_equal(update_buf, fetch_buf, UPDATE_BUF_SIZE);

exit:
	return rc;
}

static inline int
hold_object_refs(struct vos_obj_ref **refs,
		 struct daos_lru_cache *occ,
		 daos_handle_t *coh,
		 daos_unit_oid_t *oid,
		 int start, int end)
{
	int i = 0, rc = 0;

	for (i = start; i < end; i++) {
		rc = vos_obj_ref_hold(occ, *coh, *oid, &refs[i]);
		assert_int_equal(rc, 0);
	}

	return rc;
}

static void
io_oi_test(void **state)
{
	struct io_test_args	*arg = *state;
	struct vos_obj		*obj[2];
	struct vc_hdl		*co_hdl;
	daos_unit_oid_t		oid;
	int			rc = 0;

	vts_io_set_oid(&oid);
	total_oids++;

	co_hdl = vos_hdl2co(arg->ctx.tc_co_hdl);
	assert_ptr_not_equal(co_hdl, NULL);

	rc = vos_oi_lookup(co_hdl, oid, &obj[0]);
	assert_int_equal(rc, 0);

	rc = vos_oi_lookup(co_hdl, oid, &obj[1]);
	assert_int_equal(rc, 0);
}

static void
io_obj_cache_test(void **state)
{
	struct io_test_args	*arg = *state;
	struct daos_lru_cache	*occ = NULL;
	daos_unit_oid_t		oid[2];
	struct vos_obj_ref	*refs[20];
	struct vos_test_ctx	*ctx = &arg->ctx;
	int			rc, i;

	rc = vos_obj_cache_create(10, &occ);
	assert_int_equal(rc, 0);

	vts_io_set_oid(&oid[0]);
	total_oids++;

	vts_io_set_oid(&oid[1]);
	total_oids++;


	rc = hold_object_refs(refs, occ, &ctx->tc_co_hdl, &oid[0], 0, 10);
	assert_int_equal(rc, 0);

	rc = hold_object_refs(refs, occ, &ctx->tc_co_hdl, &oid[1], 10, 15);
	assert_int_equal(rc, 0);

	for (i = 0; i < 5; i++)
		vos_obj_ref_release(occ, refs[i]);
	for (i = 10; i < 15; i++)
		vos_obj_ref_release(occ, refs[i]);

	rc = hold_object_refs(refs, occ, &ctx->tc_co_hdl, &oid[1], 15, 20);
	assert_int_equal(rc, 0);

	for (i = 5; i < 10; i++)
		vos_obj_ref_release(occ, refs[i]);
	for (i = 15; i < 20; i++)
		vos_obj_ref_release(occ, refs[i]);

	vos_obj_cache_destroy(occ);
}

static void
io_multiple_dkey_test(void **state, unsigned int flags)
{
	struct io_test_args	*arg = *state;
	int			 i;
	int			 rc = 0;
	daos_epoch_t		 epoch = gen_rand_epoch();

	arg->ta_flags = flags;
	for (i = 0; i < VTS_IO_KEYS; i++) {
		rc = io_update_and_fetch_dkey(arg, epoch, epoch);
		assert_int_equal(rc, 0);
	}
}

static void
io_multiple_dkey(void **state)
{
	io_multiple_dkey_test(state, 0);
}

static void
io_multiple_dkey_ext(void **state)
{
	io_multiple_dkey_test(state, TF_REC_EXT);
}

static void
io_multiple_dkey_zc(void **state)
{
	io_multiple_dkey_test(state, TF_ZERO_COPY);
}

static void
io_multiple_dkey_zc_ext(void **state)
{
	io_multiple_dkey_test(state, TF_ZERO_COPY | TF_REC_EXT);
}

static void
io_idx_overwrite_test(void **state, unsigned int flags)
{
	struct io_test_args	*arg = *state;
	daos_epoch_t		 epoch = gen_rand_epoch();
	int			 rc = 0;

	arg->ta_flags = flags;
	rc = io_update_and_fetch_dkey(arg, epoch, epoch);
	assert_int_equal(rc, 0);

	arg->ta_flags |= TF_OVERWRITE;
	rc = io_update_and_fetch_dkey(arg, epoch, epoch);
	assert_int_equal(rc, 0);
}

static void
io_idx_overwrite(void **state)
{
	io_idx_overwrite_test(state, 0);
}

static void
io_idx_overwrite_zc(void **state)
{
	io_idx_overwrite_test(state, TF_ZERO_COPY);
}

static void
io_iter_test(void **state)
{
	struct io_test_args	*arg = *state;
	int			rc = 0;

	arg->ta_flags = 0;
	rc = io_obj_iter_test(arg);
	assert_true(rc == 0 || rc == -DER_NONEXIST);
}

static void
io_iter_test_with_anchor(void **state)
{
	struct io_test_args	*arg = *state;
	int			rc = 0;

	arg->ta_flags = TF_IT_ANCHOR;
	rc = io_obj_iter_test(arg);
	assert_true(rc == 0 || rc == -DER_NONEXIST);
}

static int
io_update_and_fetch_incorrect_dkey(struct io_test_args *arg,
				   daos_epoch_t update_epoch,
				   daos_epoch_t fetch_epoch)
{

	int			rc = 0;
	daos_iov_t		val_iov;
	daos_key_t		dkey;
	daos_key_t		akey;
	daos_recx_t		rex;
	char			dkey_buf[UPDATE_DKEY_SIZE];
	char			akey_buf[UPDATE_DKEY_SIZE];
	char			update_buf[UPDATE_BUF_SIZE];
	char			fetch_buf[UPDATE_BUF_SIZE];
	daos_vec_iod_t		vio;
	daos_sg_list_t		sgl;

	memset(&vio, 0, sizeof(vio));
	memset(&rex, 0, sizeof(rex));
	memset(&sgl, 0, sizeof(sgl));

	gen_rand_key(&dkey_buf[0], UPDATE_DKEY,  UPDATE_DKEY_SIZE);
	gen_rand_key(&akey_buf[0], UPDATE_AKEY,  UPDATE_DKEY_SIZE);
	memcpy(last_akey, akey_buf, UPDATE_AKEY_SIZE);

	daos_iov_set(&dkey, &dkey_buf[0], strlen(dkey_buf));
	daos_iov_set(&akey, &akey_buf[0], strlen(akey_buf));

	memset(update_buf, (rand() % 94) + 33, UPDATE_BUF_SIZE);
	daos_iov_set(&val_iov, &update_buf[0], UPDATE_BUF_SIZE);
	rex.rx_rsize	= val_iov.iov_len;

	sgl.sg_nr.num = 1;
	sgl.sg_iovs = &val_iov;

	rex.rx_nr	= 1;
	rex.rx_idx	= daos_hash_string_u32(dkey_buf, dkey.iov_len);
	rex.rx_idx	%= 1000000;

	vio.vd_name	= akey;
	vio.vd_recxs	= &rex;
	vio.vd_nr	= 1;

	rc = io_test_obj_update(arg, update_epoch, &dkey, &vio, &sgl);
	if (rc)
		goto exit;

	if (!(arg->ta_flags & TF_OVERWRITE))
		total_keys++;

	memset(fetch_buf, 0, UPDATE_BUF_SIZE);
	daos_iov_set(&val_iov, &fetch_buf[0], UPDATE_BUF_SIZE);

	rex.rx_rsize = DAOS_REC_ANY;

	/* Injecting an incorrect dkey for fetch! */
	memset(dkey_buf, 0, UPDATE_DKEY_SIZE);
	gen_rand_key(&dkey_buf[0], UPDATE_DKEY,  UPDATE_DKEY_SIZE);

	rc = io_test_obj_fetch(arg, fetch_epoch, &dkey, &vio, &sgl);
	if (rc)
		goto exit;
	assert_memory_equal(update_buf, fetch_buf, UPDATE_BUF_SIZE);
exit:
	return rc;
}

/**
 * NB: this test assumes arg->oid is already created and
 * used to insert in previous tests..
 * If run independently this will be treated as a new object
 * and return success
 */
static void
io_fetch_wo_object(void **state)
{
	struct io_test_args	*arg = *state;
	int			rc = 0;
	daos_iov_t		val_iov;
	daos_key_t		dkey;
	daos_key_t		akey;
	daos_recx_t		rex;
	char			dkey_buf[UPDATE_DKEY_SIZE];
	char			akey_buf[UPDATE_AKEY_SIZE];
	char			fetch_buf[UPDATE_BUF_SIZE];
	daos_vec_iod_t		vio;
	daos_sg_list_t		sgl;

	memset(&vio, 0, sizeof(vio));
	memset(&rex, 0, sizeof(rex));
	memset(&sgl, 0, sizeof(sgl));
	gen_rand_key(&dkey_buf[0], UPDATE_DKEY, UPDATE_DKEY_SIZE);
	gen_rand_key(&akey_buf[0], UPDATE_AKEY, UPDATE_AKEY_SIZE);
	daos_iov_set(&dkey, &dkey_buf[0], strlen(dkey_buf));
	daos_iov_set(&akey, &akey_buf[0], strlen(akey_buf));

	sgl.sg_nr.num = 1;
	sgl.sg_iovs = &val_iov;

	rex.rx_nr	= 1;
	rex.rx_idx	= daos_hash_string_u32(dkey_buf, dkey.iov_len);
	rex.rx_idx	%= 1000000;

	vio.vd_name	= akey;
	vio.vd_recxs	= &rex;
	vio.vd_nr	= 1;

	memset(fetch_buf, 0, UPDATE_BUF_SIZE);
	daos_iov_set(&val_iov, &fetch_buf[0], UPDATE_BUF_SIZE);

	rex.rx_rsize = DAOS_REC_ANY;
	rc = io_test_obj_fetch(arg, 1, &dkey, &vio, &sgl);
	assert_int_equal(rc, -2005);

}


static int
io_oid_iter_test(struct io_test_args *arg)
{
	vos_iter_param_t	param;
	daos_handle_t		ih;
	int			nr = 0;
	int			rc = 0;

	memset(&param, 0, sizeof(param));
	param.ip_hdl	= arg->ctx.tc_co_hdl;

	rc = vos_iter_prepare(VOS_ITER_OBJ, &param, &ih);
	if (rc != 0) {
		print_error("Failed to prepare obj iterator\n");
		return rc;
	}

	rc = vos_iter_probe(ih, NULL);
	if (rc != 0) {
		print_error("Failed to set iterator cursor: %d\n", rc);
		goto out;
	}

	while (1) {
		vos_iter_entry_t	ent;
		daos_hash_out_t		anchor;

		rc = vos_iter_fetch(ih, &ent, NULL);
		if (rc == -DER_NONEXIST) {
			print_message("Finishing obj iteration\n");
			break;
		}

		if (rc != 0) {
			print_error("Failed to fetch objid: %d\n", rc);
			goto out;
		}

		D_DEBUG(DF_VOS3, "Object ID: "DF_UOID"\n", DP_UOID(ent.ie_oid));
		nr++;

		rc = vos_iter_next(ih);
		if (rc == -DER_NONEXIST)
			break;

		if (rc != 0) {
			print_error("Failed to move cursor: %d\n", rc);
			goto out;
		}

		if (!(arg->ta_flags & TF_IT_ANCHOR))
			continue;

		rc = vos_iter_fetch(ih, &ent, &anchor);
		if (rc != 0) {
			assert_true(rc != -DER_NONEXIST);
			print_error("Failed to fetch anchor: %d\n", rc);
			goto out;
		}

		rc = vos_iter_probe(ih, &anchor);
		if (rc != 0) {
			assert_true(rc != -DER_NONEXIST);
			print_error("Failed to probe anchor: %d\n", rc);
			goto out;
		}
	}
out:
	print_message("Enumerated %d, total_oids: %d\n", nr, total_oids);
	assert_int_equal(nr, total_oids);
	vos_iter_finish(ih);
	return rc;

}



static void
io_fetch_no_exist_dkey(void **state)
{
	struct io_test_args	*arg = *state;
	int			rc;

	arg->ta_flags = 0;

	rc = io_update_and_fetch_incorrect_dkey(arg, 1, 1);
	assert_int_equal(rc, -2005);
}

static void
io_fetch_no_exist_dkey_zc(void **state)
{
	struct io_test_args	*arg = *state;
	int			rc;

	arg->ta_flags = TF_ZERO_COPY;

	rc = io_update_and_fetch_incorrect_dkey(arg, 1, 1);
	assert_int_equal(rc, -2005);
}

static void
io_fetch_no_exist_object(void **state)
{

	struct io_test_args	*arg = *state;

	arg->ta_flags = 0;

	io_fetch_wo_object(state);
}

static void
io_fetch_no_exist_object_zc(void **state)
{

	struct io_test_args	*arg = *state;

	arg->ta_flags = TF_ZERO_COPY;

	io_fetch_wo_object(state);
}

static void
io_simple_one_key_zc(void **state)
{
	struct io_test_args	*arg = *state;
	int			rc;

	arg->ta_flags = TF_ZERO_COPY;

	rc = io_update_and_fetch_dkey(arg, 1, 1);
	assert_int_equal(rc, 0);
}

static void
io_simple_one_key(void **state)
{
	struct io_test_args	*arg = *state;
	int			rc;

	arg->ta_flags = 0;

	rc = io_update_and_fetch_dkey(arg, 1, 1);
	assert_int_equal(rc, 0);
}

static void
io_simple_one_key_cross_container(void **state)
{
	struct io_test_args	*arg = *state;
	int			rc;
	daos_iov_t		val_iov;
	daos_recx_t		rex;
	char			dkey_buf[UPDATE_DKEY_SIZE];
	char			update_buf[UPDATE_BUF_SIZE];
	char			fetch_buf[UPDATE_BUF_SIZE];
	daos_vec_iod_t		vio;
	daos_sg_list_t		sgl;
	daos_dkey_t		dkey;
	daos_epoch_t		epoch = gen_rand_epoch();
	daos_unit_oid_t		l_oid;

	/* Creating an additional container */
	uuid_generate_time_safe(arg->addn_co_uuid);
	rc = vos_co_create(arg->ctx.tc_po_hdl, arg->addn_co_uuid, NULL);
	if (rc) {
		print_error("vos container creation error: %d\n", rc);
		return;
	}

	rc = vos_co_open(arg->ctx.tc_po_hdl, arg->addn_co_uuid, &arg->addn_co,
			 NULL);
	if (rc) {
		print_error("vos container open error: %d\n", rc);
		goto failed;
	}

	memset(&vio, 0, sizeof(vio));
	memset(&rex, 0, sizeof(rex));
	memset(&sgl, 0, sizeof(sgl));
	memset(dkey_buf, 0, UPDATE_DKEY_SIZE);
	memset(update_buf, 0, UPDATE_BUF_SIZE);
	daos_iov_set(&dkey, &dkey_buf[0], UPDATE_DKEY_SIZE);

	sgl.sg_nr.num = 1;
	sgl.sg_iovs = &val_iov;

	memset(update_buf, (rand() % 94) + 33, UPDATE_BUF_SIZE);
	daos_iov_set(&val_iov, &update_buf[0], UPDATE_BUF_SIZE);

	if (arg->ta_flags & TF_REC_EXT) {
		rex.rx_rsize = UPDATE_REC_SIZE;
		rex.rx_nr    = UPDATE_BUF_SIZE / UPDATE_REC_SIZE;
	} else {
		rex.rx_rsize = UPDATE_BUF_SIZE;
		rex.rx_nr    = 1;
	}
	rex.rx_idx	= daos_hash_string_u32(dkey_buf, dkey.iov_len);
	rex.rx_idx	%= 1000000;

	vio.vd_recxs	= &rex;
	vio.vd_nr	= 1;

	vts_io_set_oid(&l_oid);
	total_oids++;
	rc  = vos_obj_update(arg->ctx.tc_co_hdl,
			     arg->oid, epoch, &dkey, 1,
			     &vio, &sgl, NULL);
	if (rc) {
		print_error("Failed to update %d\n", rc);
		goto failed;
	}

	rc = vos_obj_update(arg->addn_co, l_oid, epoch,
			    &dkey, 1, &vio, &sgl, NULL);
	if (rc) {
		print_error("Failed to update %d\n", rc);
		goto failed;
	}

	memset(fetch_buf, 0, UPDATE_BUF_SIZE);
	daos_iov_set(&val_iov, &fetch_buf[0], UPDATE_BUF_SIZE);
	rex.rx_rsize = DAOS_REC_ANY;

	/**
	 * Fetch from second container with local obj id
	 * This should succeed.
	 */
	rc = vos_obj_fetch(arg->addn_co, l_oid, epoch,
			   &dkey, 1, &vio, &sgl, NULL);
	assert_memory_equal(update_buf, fetch_buf, UPDATE_BUF_SIZE);

	memset(fetch_buf, 0, UPDATE_BUF_SIZE);
	daos_iov_set(&val_iov, &fetch_buf[0], UPDATE_BUF_SIZE);
	rex.rx_rsize = DAOS_REC_ANY;

	/**
	 * Fetch the objiD used in first container
	 * from second container should throw an error
	 */
	rc = vos_obj_fetch(arg->addn_co, arg->oid, epoch,
			   &dkey, 1, &vio, &sgl, NULL);
	/* This fetch should fail */
	assert_memory_not_equal(update_buf, fetch_buf, UPDATE_BUF_SIZE);

failed:
	rc = vos_co_close(arg->addn_co, NULL);
	assert_int_equal(rc, 0);

	rc = vos_co_destroy(arg->ctx.tc_po_hdl, arg->addn_co_uuid, NULL);
	assert_int_equal(rc, 0);
}

static void
io_simple_punch(void **state)
{
	struct io_test_args	*arg = *state;
	int			rc;

	arg->ta_flags = TF_PUNCH;
	/*
	 * Punch the last updated key at a future
	 * epoch
	 */
	rc = io_update_and_fetch_dkey(arg, 10, 10);
	assert_int_equal(rc, 0);
}



static void
io_simple_near_epoch(void **state)
{
	struct io_test_args	*arg = *state;
	daos_epoch_t		epoch = gen_rand_epoch();
	int			rc;

	arg->ta_flags = 0;
	rc = io_update_and_fetch_dkey(arg, epoch, epoch + 1000);
	assert_int_equal(rc, 0);
}

static void
io_simple_near_epoch_zc(void **state)
{
	struct io_test_args	*arg = *state;
	daos_epoch_t		epoch = gen_rand_epoch();
	int			rc;

	arg->ta_flags = TF_ZERO_COPY;
	rc = io_update_and_fetch_dkey(arg, epoch, epoch + 1000);
	assert_int_equal(rc, 0);
}

static void
io_pool_overflow_test(void **state)
{
	struct io_test_args	*arg = *state;
	int			rc, i = 0;
	daos_epoch_t		epoch = gen_rand_epoch();

	vts_ctx_fini(&arg->ctx);

	rc = vts_ctx_init(&arg->ctx, VPOOL_16M);
	assert_int_equal(rc, 0);

	arg->ta_flags = 0;
	vts_io_set_oid(&arg->oid);

	for (i = 0; i < VTS_IO_KEYS; i++) {
		rc = io_update_and_fetch_dkey(arg, epoch, epoch);
		if (rc) {
			assert_int_equal(rc, -DER_NOSPACE);
			break;
		}
	}
}

static int
io_pool_overflow_teardown(void **state)
{
	struct io_test_args	*arg = *state;
	int			rc;

	vts_ctx_fini(&arg->ctx);

	rc = vts_ctx_init(&arg->ctx, VPOOL_1G);
	assert_int_equal(rc, 0);

	vts_io_set_oid(&arg->oid);
	total_oids = 1;

	*state = arg;
	return rc;
}

static int
oid_iter_test_setup(void **state)
{
	struct io_test_args	*arg = *state;
	struct vos_obj		*lobj;
	struct vc_hdl		*co_hdl;
	int			i, rc = 0;
	daos_unit_oid_t		oid[VTS_IO_OIDS];

	co_hdl = vos_hdl2co(arg->ctx.tc_co_hdl);
	assert_ptr_not_equal(co_hdl, NULL);

	for (i = 0; i < VTS_IO_OIDS; i++) {
		vts_io_set_oid(&oid[i]);
		total_oids++;
		rc = vos_oi_lookup(co_hdl, oid[i], &lobj);
		assert_int_equal(rc, 0);
	}

	return 0;
}

static void
oid_iter_test(void **state)
{
	struct io_test_args	*arg = *state;
	int			rc = 0;

	arg->ta_flags = 0;
	rc = io_oid_iter_test(arg);
	assert_true(rc == 0 || rc == -DER_NONEXIST);
}

static void
oid_iter_test_with_anchor(void **state)
{
	struct io_test_args	*arg = *state;
	int			rc = 0;

	arg->ta_flags = TF_IT_ANCHOR;
	rc = io_oid_iter_test(arg);
	assert_true(rc == 0 || rc == -DER_NONEXIST);
}




static const struct CMUnitTest io_tests[] = {
	{ "VOS201: VOS object IO index",
		io_oi_test, NULL, NULL},
	{ "VOS202: VOS object cache test",
		io_obj_cache_test, NULL, NULL},
	{ "VOS203: Simple update/fetch/verify test",
		io_simple_one_key, NULL, NULL},
	{ "VOS204: Simple Punch test",
		io_simple_punch, NULL, NULL},
	{ "VOS205: Simple update/fetch/verify test (for dkey) with zero-copy",
		io_simple_one_key_zc, NULL, NULL},
	{ "VOS206: Simple near-epoch retrieval test",
		io_simple_near_epoch, NULL, NULL},
	{ "VOS207: Simple near-epoch retrieval test with zero-copy",
		io_simple_near_epoch_zc, NULL, NULL},
	{ "VOS208: 100K update/fetch/verify test",
		io_multiple_dkey, NULL, NULL},
	{ "VOS208.1: 100K update/fetch/verify test (extent)",
		io_multiple_dkey_ext, NULL, NULL},
	{ "VOS209: 100k update/fetch/verify test with zero-copy",
		io_multiple_dkey_zc, NULL, NULL},
	{ "VOS209.1: 100k update/fetch/verify test with zero-copy (extent)",
		io_multiple_dkey_zc_ext, NULL, NULL},
	{ "VOS210: overwrite test",
		io_idx_overwrite, NULL, NULL},
	{ "VOS211: overwrite test with zero-copy",
		io_idx_overwrite_zc, NULL, NULL},
	{ "VOS212: KV Iter tests (for dkey)",
		io_iter_test, NULL, NULL},
	{ "VOS213: KV Iter tests with anchor (for dkey)",
		io_iter_test_with_anchor, NULL, NULL},
	{ "VOS214: Object iter test (for oid)",
		oid_iter_test, oid_iter_test_setup, NULL},
	{ "VOS215: Object iter test with anchor (for oid)",
		oid_iter_test_with_anchor, oid_iter_test_setup, NULL},
	{ "VOS216: Same Obj ID on two containers (obj_cache test)",
		io_simple_one_key_cross_container, NULL, NULL},
	{ "VOS217: Fetch from non existent object",
		io_fetch_no_exist_object, NULL, NULL},
	{ "VOS218: Fetch from non existent object with zero-copy",
		io_fetch_no_exist_object_zc, NULL, NULL},
	{ "VOS219: Fetch from non existent dkey",
		io_fetch_no_exist_dkey, NULL, NULL},
	{ "VOS220: Fetch from non existent dkey with zero-copy",
		io_fetch_no_exist_dkey_zc, NULL, NULL},
	{ "VOS221: Space overflow negative error test",
		io_pool_overflow_test, NULL, io_pool_overflow_teardown},
};

int
run_io_test(void)
{
	return cmocka_run_group_tests_name("VOS IO tests", io_tests,
					   setup, teardown);
}
