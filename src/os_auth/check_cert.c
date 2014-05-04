/* @(#) $Id: ./src/os_auth/check_cert.c, 2014/04/25 mweigel Exp $
 */

/* Copyright (C) 2010 Trend Micro Inc.
 * All rights reserved.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 *
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 *
 */

#ifdef USE_OPENSSL

#include "shared.h"
#include "check_cert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Could be replaced with X509_check_host() in future but this is only available
 * in openssl 1.0.2.
 */
int check_x509_cert(SSL *ssl, char *manager)
{
    X509 *cert = NULL;
    int verified = VERIFY_FALSE;

    if(!(cert = SSL_get_peer_certificate(ssl)))
        goto CERT_CHECK_ERROR;

    /* Check for a matching subject alt name entry in the extensions first and
     * if no match is found there then check the subject CN.
     */
    debug1("%s: DEBUG: Checking certificate's subject alternative names.", ARGV0);
    if((verified = check_subject_alt_names(cert, manager)) == VERIFY_ERROR)
        goto CERT_CHECK_ERROR;

    if(verified == VERIFY_FALSE)
    {
        debug1("%s: DEBUG: No matching subject alternative names found. Checking common name.", ARGV0);
        if((verified = check_subject_cn(cert, manager)) == VERIFY_ERROR)
            goto CERT_CHECK_ERROR;
    }

    X509_free(cert);

    return verified;

CERT_CHECK_ERROR:
    if (cert)
        X509_free(cert);

    return VERIFY_ERROR;
}

/* Loop through all the subject_alt_name entries until we find a match or
 * an error occurs. Only entries containing a normal domain name or IP
 * address are considered.
 */
int check_subject_alt_names(X509 *cert, char *manager)
{
    GENERAL_NAMES *names = NULL;
    int result = VERIFY_FALSE;
    int i = 0;

    if((names = X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL)))
    {
        for(i = 0; i < sk_GENERAL_NAME_num(names) && result == VERIFY_FALSE; i++)
        {
            GENERAL_NAME *name = sk_GENERAL_NAME_value(names, i);

            if(name->type == GEN_DNS)
                result = check_hostname(name->d.dNSName, manager);
            else if(name->type == GEN_IPADD)
                result = check_ipaddr(name->d.iPAddress, manager);
        }

        GENERAL_NAMES_free(names);
    }

    return result;
}

/* Loop through all the common name entries until we find a match or
 * an error occurs.
 */
int check_subject_cn(X509 *cert, char *manager)
{
    X509_NAME *name = NULL;
    int result = VERIFY_FALSE;
    int i = 0;

    if((name = X509_get_subject_name(cert)))
    {
        while((i = X509_NAME_get_index_by_NID(name, NID_commonName, i)) >= 0 && result == VERIFY_FALSE)
        {
            X509_NAME_ENTRY *ne = X509_NAME_get_entry(name, i);
            result = check_hostname(X509_NAME_ENTRY_get_data(ne), manager);
        }
    }

    return result;
}

/* Determine whether a string found in a subject alt name or common name
 * field matches the manager's name specified on the command line. The
 * domain name from the certificate and the domain name from the command
 * line are broken down into a sequence of labels before being validated
 * and compared. Matching is case insensitive and basic wildcard matching
 * is supported.
 */
int check_hostname(ASN1_STRING *cert_astr, char *manager)
{
    label c_labels[DNS_MAX_LABELS];
    label m_labels[DNS_MAX_LABELS];
    int c_label_num = 0;
    int m_label_num = 0;
    int wildcard_cert = 0;
    int i = 0;
    char *cert_cstr = NULL;

    ASN1_STRING_to_UTF8((unsigned char **)&cert_cstr, cert_astr);
    if(!cert_cstr)
        return VERIFY_ERROR;

    c_label_num = label_array(cert_cstr, c_labels);
    m_label_num = label_array(manager, m_labels);
    OPENSSL_free(cert_cstr);

    /* Check that we have an appropriate number of labels and that the name
     * from the certificate and the name given on the command line have
     * the same number of labels.
     */
    if(m_label_num <= 0 || c_label_num <= 0)
        return VERIFY_FALSE;

    if(m_label_num != c_label_num)
        return VERIFY_FALSE;

    /* Wildcards are accepted in the first label only. Partial wildcard
     * matching is not supported.
     */
    if(label_valid(&m_labels[0]) && !strcmp(c_labels[0].text, "*"))
        wildcard_cert = 1;

    /* Validate and match all labels.
     */
    for(i = wildcard_cert; i < m_label_num; i++)
    {
        if(!label_valid(&m_labels[i]))
            return VERIFY_FALSE;

        if(!label_match(&m_labels[i], &c_labels[i]))
            return VERIFY_FALSE;
    }

    return VERIFY_TRUE;
}

int check_ipaddr(ASN1_STRING *cert_astr, char *manager)
{
    struct sockaddr_in iptest;
    struct sockaddr_in6 iptest6;

    memset(&iptest, 0, sizeof(iptest));
    memset(&iptest6, 0, sizeof(iptest6));

    if(inet_pton(AF_INET, manager, &iptest.sin_addr) == 1)
    {
        if(cert_astr->length == 4 && !memcmp(cert_astr->data, (const void *)&iptest.sin_addr, 4))
            return VERIFY_TRUE;
    }
    else if(inet_pton(AF_INET6, manager, &iptest6.sin6_addr) == 1)
    {
        if(cert_astr->length == 16 && !memcmp(cert_astr->data, (const void *)&iptest6.sin6_addr, 16))
            return VERIFY_TRUE;
    }

    return VERIFY_FALSE;
}

/* Separate a domain name into a sequence of labels and return the number
 * of labels found.
 */
int label_array(const char *domain_name, label result[DNS_MAX_LABELS])
{
    int label_count = 0;
    const char *label_start = domain_name;
    const char *label_end = domain_name;

    do
    {
        if(label_count == DNS_MAX_LABELS)
            return VERIFY_FALSE;

        if(*label_end == '.' || *label_end == '\0')
        {
            label *new_label = &result[label_count];

            if((new_label->len = label_end - label_start) > DNS_MAX_LABEL_LEN)
                return VERIFY_FALSE;

            strncpy(new_label->text, label_start, new_label->len);
            new_label->text[new_label->len] = '\0';

            label_start = label_end + 1;
            label_count++;
        }
    }
    while(*label_end++ != '\0');

    /* If the length of the last label is zero ignore it. This is the only
     * valid position for a label of length zero which occurs when a FQDN
     * is given.
     */
    return (result[label_count - 1].len > 0) ? label_count : label_count - 1;
}

/* Validate a label according to the guidelines in RFC 1035.
 */
int label_valid(const label *label)
{
    int i;

    if(label->len == 0 || label->len > DNS_MAX_LABEL_LEN)
        return VERIFY_FALSE;

    if(!isalpha(label->text[0]) || !isalnum(label->text[label->len - 1]))
        return VERIFY_FALSE;

    for(i = 0; i < label->len; i++)
    {
        if(!isalnum(label->text[i]) && label->text[i] != '-')
            return VERIFY_FALSE;
    }

    return VERIFY_TRUE;
}

/* Compare two labels and determine whether they match.
 */
int label_match(const label *label1, const label *label2)
{
    int i;

    if(label1->len != label2->len)
        return VERIFY_FALSE;

    for(i = 0; i < label1->len; i++)
    {
        if(tolower(label1->text[i]) != tolower(label2->text[i]))
            return VERIFY_FALSE;
    }

    return VERIFY_TRUE;
}

#endif /* USE_OPENSSL */

