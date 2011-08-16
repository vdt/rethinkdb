#include "serializer/semantic_checking_internal.hpp"

template<class inner_serializer_t>
uint32_t semantic_checking_serializer_t<inner_serializer_t>::
compute_crc(const void *buf) {
    boost::crc_32_type crc_computer;
    // We need to not crc BLOCK_META_DATA_SIZE because it's
    // internal to the serializer.
    crc_computer.process_bytes(buf, get_block_size().value());
    return crc_computer.checksum();
}

template<class inner_serializer_t>
void semantic_checking_serializer_t<inner_serializer_t>::
update_block_info(block_id_t block_id, scs_block_info_t info) {
    blocks.set(block_id, info);
    scs_persisted_block_info_t buf;
    buf.block_id = block_id;
    buf.block_info = info;
    int res = write(semantic_fd, &buf, sizeof(buf));
    guarantee_err(res == sizeof(buf), "Could not write data to semantic checker file");
}

template<class inner_serializer_t>
struct semantic_checking_serializer_t<inner_serializer_t>::scs_block_token_t : public serializer_t::block_token_t {
    scs_block_token_t(semantic_checking_serializer_t *ser, block_id_t block_id, const scs_block_info_t &info,
                      boost::shared_ptr<block_token_t> tok)
        : serializer(ser), block_id(block_id), info(info), inner_token(tok) {
        rassert(inner_token, "scs_block_token wrapping null token");
    }
    semantic_checking_serializer_t *serializer;
    block_id_t block_id;    // NULL_BLOCK_ID if not associated with a block id
    scs_block_info_t info;      // invariant: info.state != scs_block_info_t::state_deleted
    boost::shared_ptr<block_token_t> inner_token;
};

template<class inner_serializer_t>
boost::shared_ptr<serializer_t::block_token_t> semantic_checking_serializer_t<inner_serializer_t>::
wrap_token(block_id_t block_id, scs_block_info_t info, boost::shared_ptr<block_token_t> inner_token) {
    return boost::shared_ptr<block_token_t>(new scs_block_token_t(this, block_id, info, inner_token));
}

template<class inner_serializer_t>
void semantic_checking_serializer_t<inner_serializer_t>::
create(dynamic_config_t config, private_dynamic_config_t private_config, static_config_t static_config) {
    inner_serializer_t::create(config, private_config, static_config);
}

template<class inner_serializer_t>
semantic_checking_serializer_t<inner_serializer_t>::
semantic_checking_serializer_t(dynamic_config_t config, private_dynamic_config_t private_config)
    : inner_serializer(config, private_config),
      last_index_write_started(0), last_index_write_finished(0),
      semantic_fd(-1)
{
    semantic_fd = open(private_config.semantic_filename.c_str(),
        O_RDWR | O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO);
    if (semantic_fd == INVALID_FD)
        fail_due_to_user_error("Inaccessible semantic checking file: \"%s\": %s", private_config.semantic_filename.c_str(), strerror(errno));

    // fill up the blocks from the semantic checking file
    int res = -1;
    do {
        scs_persisted_block_info_t buf;
        res = read(semantic_fd, &buf, sizeof(buf));
        guarantee_err(res != -1, "Could not read from the semantic checker file");
        if(res == sizeof(scs_persisted_block_info_t)) {
            blocks.set(buf.block_id, buf.block_info);
        }
    } while(res == sizeof(scs_persisted_block_info_t));
}

template<class inner_serializer_t>
semantic_checking_serializer_t<inner_serializer_t>::
~semantic_checking_serializer_t() {
    close(semantic_fd);
}

template<class inner_serializer_t>
void semantic_checking_serializer_t<inner_serializer_t>::
check_existing(const char *db_path, check_callback_t *cb) {
    inner_serializer_t::check_existing(db_path, cb);
}

template<class inner_serializer_t>
void *semantic_checking_serializer_t<inner_serializer_t>::
malloc() { return inner_serializer.malloc(); }

template<class inner_serializer_t>
void *semantic_checking_serializer_t<inner_serializer_t>::
clone(void *data) { return inner_serializer.clone(data); }

template<class inner_serializer_t>
void semantic_checking_serializer_t<inner_serializer_t>::
free(void *ptr) { inner_serializer.free(ptr); }

template<class inner_serializer_t>
file_t::account_t *semantic_checking_serializer_t<inner_serializer_t>::
make_io_account(int priority, int outstanding_requests_limit) {
    return inner_serializer.make_io_account(priority, outstanding_requests_limit);
}

template<class inner_serializer_t>
boost::shared_ptr<serializer_t::block_token_t> semantic_checking_serializer_t<inner_serializer_t>::
index_read(block_id_t block_id) {
    scs_block_info_t info = blocks.get(block_id);
    boost::shared_ptr<block_token_t> result = inner_serializer.index_read(block_id);
    guarantee(info.state != scs_block_info_t::state_deleted || !result,
              "Cache asked for a deleted block, and serializer didn't complain.");
    return result ? wrap_token(block_id, info, result) : result;
}

/* For reads, we check to make sure that the data we get back in the read is
consistent with what was last written there. */

template<class inner_serializer_t>
void semantic_checking_serializer_t<inner_serializer_t>::
read_check_state(scs_block_token_t *token, const void *buf) {
    uint32_t actual_crc = compute_crc(buf);
    scs_block_info_t &expected = token->info;

    switch (expected.state) {
        case scs_block_info_t::state_unknown: {
            /* We don't know what this block was supposed to contain, so we can't do any
            verification */
            break;
        }

        case scs_block_info_t::state_have_crc: {
#ifdef SERIALIZER_DEBUG_PRINT
            printf("Read %u: %u\n", token->block_id, actual_crc);
#endif
            guarantee(expected.crc == actual_crc, "Serializer returned bad value for block ID %u", token->block_id);
            break;
        }

        case scs_block_info_t::state_deleted:
        default:
            unreachable();
    }
}

template<class inner_serializer_t>
struct semantic_checking_serializer_t<inner_serializer_t>::reader_t : public iocallback_t {
    scs_block_token_t *token;
    void *buf;
    iocallback_t *callback;

    reader_t(scs_block_token_t *token, void *buf, iocallback_t *cb) : token(token), buf(buf), callback(cb) {}

    void on_io_complete() {
        token->serializer->read_check_state(token, buf);
        if (callback) callback->on_io_complete();
        delete this;
    }
};

template<class inner_serializer_t>
void semantic_checking_serializer_t<inner_serializer_t>::
block_read(boost::shared_ptr<block_token_t> token_, void *buf, file_t::account_t *io_account, iocallback_t *callback) {
    scs_block_token_t *token = dynamic_cast<scs_block_token_t*>(token_.get());
    guarantee(token, "bad token");
#ifdef SERIALIZER_DEBUG_PRINT
    printf("Reading %u\n", token->block_id);
#endif
    reader_t *reader = new reader_t(token, buf, callback);
    inner_serializer.block_read(token->inner_token, buf, io_account, reader);
}

template<class inner_serializer_t>
void semantic_checking_serializer_t<inner_serializer_t>::
block_read(boost::shared_ptr<block_token_t> token_, void *buf, file_t::account_t *io_account) {
    scs_block_token_t *token = dynamic_cast<scs_block_token_t*>(token_.get());
    guarantee(token, "bad token");
#ifdef SERIALIZER_DEBUG_PRINT
    printf("Reading %u\n", token->block_id);
#endif
    inner_serializer.block_read(token->inner_token, buf, io_account);
    read_check_state(token, buf);
}

template<class inner_serializer_t>
block_sequence_id_t semantic_checking_serializer_t<inner_serializer_t>::
get_block_sequence_id(block_id_t block_id, const void* buf) {
    // TODO: Implement some checking for this operation
    return inner_serializer.get_block_sequence_id(block_id, buf);
}

template<class inner_serializer_t>
void semantic_checking_serializer_t<inner_serializer_t>::
index_write(const std::vector<index_write_op_t>& write_ops, file_t::account_t *io_account) {
    std::vector<index_write_op_t> inner_ops;
    inner_ops.reserve(write_ops.size());

    for (size_t i = 0; i < write_ops.size(); ++i) {
        index_write_op_t op = write_ops[i];
        scs_block_info_t info;
        if (op.token) {
            boost::shared_ptr<block_token_t> tok = op.token.get();
            if (tok) {
                scs_block_token_t *token = dynamic_cast<scs_block_token_t*>(tok.get());
                // Fix the op to point at the inner serializer's block token.
                op.token = token->inner_token;
                info = token->info;
                guarantee(token->block_id == op.block_id,
                          "indexing token with block id %u under block id %u", token->block_id, op.block_id);
            }
        }

        if (op.delete_bit && op.delete_bit.get()) {
            info.state = scs_block_info_t::state_deleted;
#ifdef SERIALIZER_DEBUG_PRINT
            printf("Setting delete bit for %u\n", op.block_id);
#endif
        } else if (info.state == scs_block_info_t::state_have_crc) {
#ifdef SERIALIZER_DEBUG_PRINT
            printf("Indexing under block id %u: %u\n", op.block_id, info.crc);
#endif
        }

        // Update the info and add it to the semantic checker file.
        update_block_info(op.block_id, info);
        // Add possibly modified op to the op vector
        inner_ops.push_back(op);
    }

    int our_index_write = ++last_index_write_started;
    inner_serializer.index_write(inner_ops, io_account);
    guarantee(last_index_write_finished == our_index_write - 1, "Serializer completed index_writes in the wrong order");
    last_index_write_finished = our_index_write;
}

template<class inner_serializer_t>
boost::shared_ptr<serializer_t::block_token_t> semantic_checking_serializer_t<inner_serializer_t>::
wrap_buf_token(block_id_t block_id, const void *buf, boost::shared_ptr<block_token_t> inner_token) {
    return wrap_token(block_id, scs_block_info_t(compute_crc(buf)), inner_token);
}

template<class inner_serializer_t>
boost::shared_ptr<serializer_t::block_token_t> semantic_checking_serializer_t<inner_serializer_t>::
block_write(const void *buf, block_id_t block_id, file_t::account_t *io_account, iocallback_t *cb) {
    return wrap_buf_token(block_id, buf, inner_serializer.block_write(buf, block_id, io_account, cb));
}

template<class inner_serializer_t>
boost::shared_ptr<serializer_t::block_token_t> semantic_checking_serializer_t<inner_serializer_t>::
block_write(const void *buf, file_t::account_t *io_account, iocallback_t *cb) {
    return wrap_buf_token(NULL_BLOCK_ID, buf, inner_serializer.block_write(buf, io_account, cb));
}

template<class inner_serializer_t>
boost::shared_ptr<serializer_t::block_token_t> semantic_checking_serializer_t<inner_serializer_t>::
block_write(const void *buf, block_id_t block_id, file_t::account_t *io_account) {
    return wrap_buf_token(block_id, buf, inner_serializer.block_write(buf, block_id, io_account));
}

template<class inner_serializer_t>
boost::shared_ptr<serializer_t::block_token_t> semantic_checking_serializer_t<inner_serializer_t>::
block_write(const void *buf, file_t::account_t *io_account) {
    return wrap_buf_token(NULL_BLOCK_ID, buf, inner_serializer.block_write(buf, io_account));
}

template<class inner_serializer_t>
block_size_t semantic_checking_serializer_t<inner_serializer_t>::
get_block_size() { return inner_serializer.get_block_size(); }

template<class inner_serializer_t>
block_id_t semantic_checking_serializer_t<inner_serializer_t>::
max_block_id() { return inner_serializer.max_block_id(); }

template<class inner_serializer_t>
repli_timestamp_t semantic_checking_serializer_t<inner_serializer_t>::
get_recency(block_id_t id) { return inner_serializer.get_recency(id); }

template<class inner_serializer_t>
bool semantic_checking_serializer_t<inner_serializer_t>::
get_delete_bit(block_id_t id) {
    // FIXME: tests seems to indicate that this code is broken. I don't know why, but it is. @rntz
    bool bit = inner_serializer.get_delete_bit(id);
    scs_block_info_t::state_t state = blocks.get(id).state;
    // If we know what the state is, it should be consistent with the delete bit.
    if (state != scs_block_info_t::state_unknown)
        guarantee(bit == (state == scs_block_info_t::state_deleted),
                  "serializer returned incorrect delete bit for block id %u", id);
    return bit;
}

template<class inner_serializer_t>
void semantic_checking_serializer_t<inner_serializer_t>::
register_read_ahead_cb(UNUSED read_ahead_callback_t *cb) {
    // Ignore this, it might make the checking ineffective...
}

template<class inner_serializer_t>
void semantic_checking_serializer_t<inner_serializer_t>::
unregister_read_ahead_cb(UNUSED read_ahead_callback_t *cb) { }

template<class inner_serializer_t>
bool semantic_checking_serializer_t<inner_serializer_t>::
disable_gc(gc_disable_callback_t *cb) { return inner_serializer.disable_gc(cb); }

template<class inner_serializer_t>
void semantic_checking_serializer_t<inner_serializer_t>::
enable_gc() { return inner_serializer.enable_gc(); }