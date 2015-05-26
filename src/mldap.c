/*
 * Copyright (C) 2015  bind-dyndb-ldap authors; see COPYING for license
 *
 * Meta-database for LDAP-specific information which are not represented in
 * DNS data.
 */

#include <ldap.h>
#include <stddef.h>
#include <uuid/uuid.h>

#include <isc/boolean.h>
#include <isc/net.h>
#include <isc/result.h>
#include <isc/util.h>

#include <dns/db.h>
#include <dns/enumclass.h>
#include <dns/name.h>
#include <dns/types.h>
#include <dns/update.h>

#include "ldap_entry.h"
#include "metadb.h"
#include "mldap.h"
#include "util.h"

/* name "ldap.uuid." */
static unsigned char uuid_rootname_ndata[]
	= { 4, 'u', 'u', 'i', 'd', 4, 'l', 'd', 'a', 'p', 0 };
static unsigned char uuid_rootname_offsets[] = { 0, 5, 10 };
static dns_name_t uuid_rootname =
{
	DNS_NAME_MAGIC,
	uuid_rootname_ndata,
	sizeof(uuid_rootname_ndata),
	sizeof(uuid_rootname_offsets),
	DNS_NAMEATTR_READONLY | DNS_NAMEATTR_ABSOLUTE,
	uuid_rootname_offsets,
	NULL,
	{ (void *)-1, (void *)-1 },
	{ NULL, NULL }
};

struct mldapdb {
	isc_mem_t	*mctx;
	metadb_t	*mdb;
	isc_uint32_t	generation;
};


isc_result_t
mldap_new(isc_mem_t *mctx, mldapdb_t **mldapp) {
	isc_result_t result;
	mldapdb_t *mldap = NULL;

	REQUIRE(mldapp != NULL && *mldapp == NULL);

	CHECKED_MEM_GET_PTR(mctx, mldap);
	ZERO_PTR(mldap);
	isc_mem_attach(mctx, &mldap->mctx);

	CHECK(metadb_new(mctx, &mldap->mdb));
	mldap->generation = 1;

	*mldapp = mldap;
	return result;

cleanup:
	metadb_destroy(&mldap->mdb);
	MEM_PUT_AND_DETACH(mldap);
	return result;
}

void
mldap_destroy(mldapdb_t **mldapp) {
	mldapdb_t *mldap;

	REQUIRE(mldapp != NULL && *mldapp != NULL);

	mldap = *mldapp;
	if (mldap == NULL)
		return;

	metadb_destroy(&mldap->mdb);
	MEM_PUT_AND_DETACH(mldap);

	*mldapp = NULL;
}


isc_result_t
mldap_newversion(mldapdb_t *mldap) {
	return metadb_newversion(mldap->mdb);
}

void
mldap_closeversion(mldapdb_t *mldap, isc_boolean_t commit) {
	return metadb_closeversion(mldap->mdb, commit);
}

/**
 * Atomically increment MetaLDAP generation number.
 */
void mldap_cur_generation_bump(mldapdb_t *mldap) {
	isc_uint32_t oldgen;
	isc_uint32_t curgen;
	isc_uint32_t newgen;

	REQUIRE(mldap != NULL);

	curgen = isc_atomic_cmpxchg((isc_int32_t *)&mldap->generation, 0, 0);
	do {
		oldgen = curgen;
		newgen = dns_update_soaserial(oldgen, dns_updatemethod_increment);
		curgen = isc_atomic_cmpxchg((isc_int32_t *)&mldap->generation, oldgen, newgen);
	} while (curgen != oldgen);
}

/**
 * Get current MetaLDAP generation number.
 *
 * Generation numbers have to be compared using isc_serial_* functions.
 */
isc_uint32_t
mldap_cur_generation_get(mldapdb_t *mldap) {
	return isc_atomic_cmpxchg((isc_int32_t *)&mldap->generation, 0, 0);
}

/**
 * Convert UUID to "01234567-89ab-cdef-0123-456789abcdef.uuid.ldap." DNS name.
 *
 * @param[in]  beruuid
 * @param[out] nameuuid
 */
void
ldap_uuid_to_mname(struct berval *beruuid, dns_name_t *nameuuid) {
	/* UUID string representation according to RFC 4122 section 3 */
	char label_buf[sizeof("01234567-89ab-cdef-0123-456789abcdef") + 1];
	/* uncompressed label format, length 36 octets; RFC 1034 section 3.1 */
	label_buf[0] = 36;

	isc_region_t label_reg;
	label_reg.base = (unsigned char *)label_buf;
	label_reg.length = sizeof(label_buf) - 1; /* omit final \0 */

	dns_name_t relative_name;
	DNS_NAME_INIT(&relative_name, NULL);

	/* RFC 4530 section 2.1 format = 16 octets is required */
	REQUIRE(beruuid != NULL && beruuid->bv_len == 16);

	/* fill-in string representation into label buffer */
	uuid_unparse((*(const uuid_t *) beruuid->bv_val), label_buf + 1);
	dns_name_fromregion(&relative_name, &label_reg);

	INSIST(dns_name_concatenate(&relative_name, &uuid_rootname,
				    nameuuid, NULL) == ISC_R_SUCCESS);

	return;
}

STATIC_ASSERT((sizeof(ldap_entryclass_t) <= sizeof(struct in6_addr)), \
		"ldap_entryclass_t is too big for AAAA rdata type");
/**
 * ldap_entryclass_t is stored inside AAAA record type
 */
static isc_result_t
mldap_class_store(ldap_entryclass_t class, metadb_node_t *node) {
	unsigned char buff[sizeof(struct in6_addr)];
	isc_region_t region = { .base = buff, .length = sizeof(buff) };
	dns_rdata_t rdata;

	dns_rdata_init(&rdata);
	memset(buff, 0, sizeof(buff));

	/* Bytes should be in network-order but we do not care because:
	 * 1) It is used only internally.  2) Class is unsigned char. */
	memcpy(buff, &class, sizeof(class));
	dns_rdata_fromregion(&rdata, dns_rdataclass_in, dns_rdatatype_aaaa,
			     &region);

	return metadb_rdata_store(&rdata, node);
}

isc_result_t
mldap_class_get(metadb_node_t *node, ldap_entryclass_t *classp) {
	isc_result_t result;
	dns_rdataset_t rdataset;
	dns_rdata_t rdata;
	isc_region_t region;

	REQUIRE(classp != NULL);

	dns_rdata_init(&rdata);
	dns_rdataset_init(&rdataset);

	CHECK(metadb_rdataset_get(node, dns_rdatatype_aaaa, &rdataset));
	dns_rdataset_current(&rdataset, &rdata);
	dns_rdata_toregion(&rdata, &region);
	/* Bytes should be in network-order but we do not care because:
	 * 1) It is used only internally.  2) Class is unsigned char. */
	memcpy(classp, region.base, sizeof(*classp));

cleanup:
	if (dns_rdataset_isassociated(&rdataset))
		dns_rdataset_disassociate(&rdataset);
	return result;
}


STATIC_ASSERT((sizeof(((mldapdb_t *)0)->generation) == sizeof(struct in_addr)), \
		"mldapdb_t->generation is too big for A rdata type");
/**
 * mldapdb_t->generation is stored inside A record type
 */
static isc_result_t
mldap_generation_store(mldapdb_t *mldap, metadb_node_t *node) {
	isc_result_t result;
	unsigned char buff[sizeof(mldap->generation)];
	isc_region_t region = { .base = buff, .length = sizeof(buff) };
	dns_rdata_t rdata;
	isc_uint32_t generation;

	STATIC_ASSERT((sizeof(((mldapdb_t *)0)->generation) == sizeof(generation)), \
		       "mldapdb_t->generation and local generation size does not match");

	dns_rdata_init(&rdata);

	/* Bytes should be in network-order but we do not care because:
	 * 1) It is used only internally and always compared on this machine. */
	generation = mldap_cur_generation_get(mldap);
	memcpy(buff, &generation, sizeof(generation));
	dns_rdata_fromregion(&rdata, dns_rdataclass_in, dns_rdatatype_a, &region);
	CHECK(metadb_rdata_store(&rdata, node));

cleanup:
	return result;
}

/**
 * FQDN and zone name are stored inside RP record type
 */
isc_result_t
mldap_dnsname_store(dns_name_t *fqdn, dns_name_t *zone, metadb_node_t *node) {
	isc_result_t result;
	dns_rdata_rp_t rp;
	dns_rdata_t rdata;
	unsigned char wirebuf[2 * DNS_NAME_MAXWIRE];
	isc_buffer_t rdatabuf;

	REQUIRE(fqdn != NULL);
	REQUIRE(zone != NULL);

	dns_rdata_init(&rdata);
	DNS_RDATACOMMON_INIT(&rp, dns_rdatatype_rp, dns_rdataclass_in);
	isc_buffer_init(&rdatabuf, wirebuf, sizeof(wirebuf));

	/* Bytes should be in network-order but we do not care because:
	 * 1) It is used only internally and always compared on this machine. */
	rp.mail = *fqdn;
	rp.text = *zone;
	CHECK(dns_rdata_fromstruct(&rdata, dns_rdataclass_in, dns_rdatatype_rp,
				   &rp, &rdatabuf));
	CHECK(metadb_rdata_store(&rdata, node));

cleanup:
	return result;
}

/**
 * Retrieve FQDN and zone name from RP record in metaDB.
 * @param[in]  node
 * @param[out] fqdn
 * @param[out] zone
 *
 * @pre DNS names fqdn and zone have dedicated buffer.
 */
isc_result_t
mldap_dnsname_get(metadb_node_t *node, dns_name_t *fqdn, dns_name_t *zone) {
	isc_result_t result;
	dns_rdata_rp_t rp;
	dns_rdataset_t rdataset;
	dns_rdata_t rdata;

	REQUIRE(fqdn != NULL);
	REQUIRE(zone != NULL);

	dns_rdataset_init(&rdataset);
	dns_rdata_init(&rdata);

	CHECK(metadb_rdataset_get(node, dns_rdatatype_rp, &rdataset));
	dns_rdataset_current(&rdataset, &rdata);
	CHECK(dns_rdata_tostruct(&rdata, &rp, NULL));
	CHECK(dns_name_copy(&rp.mail, fqdn, NULL));
	CHECK(dns_name_copy(&rp.text, zone, NULL));

cleanup:
	if (dns_rdataset_isassociated(&rdataset))
		dns_rdataset_disassociate(&rdataset);
	return result;
}

/**
 * Store information from LDAP entry into meta-database.
 */
isc_result_t
mldap_entry_create(ldap_entry_t *entry, mldapdb_t *mldap, metadb_node_t **nodep) {
	isc_result_t result;
	metadb_node_t *node = NULL;
	DECLARE_BUFFERED_NAME(mname);

	REQUIRE(nodep != NULL && *nodep == NULL);

	INIT_BUFFERED_NAME(mname);

	ldap_uuid_to_mname(entry->uuid, &mname);
	CHECK(metadb_writenode_create(mldap->mdb, &mname, &node));

	CHECK(mldap_class_store(entry->class, node));
	CHECK(mldap_generation_store(mldap, node));

	*nodep = node;

cleanup:
	if (result != ISC_R_SUCCESS)
		metadb_node_close(&node);
	return result;
}

/**
 * Open metaLDAP entry for reading.
 * All notes about metadb_readnode_open() apply equally here.
 */
isc_result_t
mldap_entry_read(mldapdb_t *mldap, struct berval *uuid, metadb_node_t **nodep) {
	DECLARE_BUFFERED_NAME(mname);

	INIT_BUFFERED_NAME(mname);

	ldap_uuid_to_mname(uuid, &mname);

	return metadb_readnode_open(mldap->mdb, &mname, nodep);
}

/**
 * Delete metaLDAP entry.
 * All notes about metadb_writenode_open() apply equally here.
 */
isc_result_t
mldap_entry_delete(mldapdb_t *mldap, struct berval *uuid) {
	isc_result_t result;
	metadb_node_t *node = NULL;
	DECLARE_BUFFERED_NAME(mname);

	INIT_BUFFERED_NAME(mname);

	ldap_uuid_to_mname(uuid, &mname);

	CHECK(metadb_writenode_open(mldap->mdb, &mname, &node));
	CHECK(metadb_node_delete(&node));

cleanup:
	return result;
}
