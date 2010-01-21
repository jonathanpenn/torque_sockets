
/// All data associated with the negotiation of the connection
struct connection_parameters
{
	bool _is_initiator; ///< True if this host initiated the arranged connection.
	bool _is_arranged; ///< True if this is an arranged connection
	array<address> _possible_addresses; ///< List of possible addresses for the remote host in an arranged connection.

	bool _puzzle_retried; ///< True if a puzzle solution was already rejected by the server once.
	nonce _nonce; ///< Unique nonce generated for this connection to send to the server.
	nonce _server_nonce; ///< Unique nonce generated by the server for the connection.
	uint32 _puzzle_difficulty; ///< Difficulty of the client puzzle solved by this client.
	uint32 _puzzle_solution; ///< Solution to the client puzzle the server sends to the client.
	uint32 _client_identity; ///< The client identity as computed by the remote host.
	uint32 _puzzle_request_index; ///< The index of the puzzle solver thread queue request.
	ref_ptr<asymmetric_key> _public_key; ///< The public key of the remote host.
	ref_ptr<asymmetric_key> _private_key;///< The private key for this connection.  May be generated on the connection attempt.
	byte_buffer_ptr _shared_secret; ///< The shared secret key 
	byte_buffer_ptr _arranged_secret; ///< The shared secret as arranged by the connection intermediary.

	uint8 _symmetric_key[symmetric_cipher::key_size]; ///< The symmetric key for the connection, generated by the client
	uint8 _init_vector[symmetric_cipher::key_size]; ///< The init vector, generated by the server
	
	byte_buffer_ptr connect_data;

	connection_parameters()
	{
		_is_initiator = false;
		_puzzle_retried = false;
		_is_arranged = false;
	}
};

//----------------------------------------------------------------------------
/// TNP network connection class.
///
/// connection is the base class for the connection classes in TNL. It implements a
/// notification protocol on the unreliable packet transport of UDP (via the TNL::Net layer).
/// connection manages the flow of packets over the network, and calls its subclasses
/// to read and write packet data, as well as handle packet delivery notification.
///
/// Because string data can easily soak up network bandwidth, for
/// efficiency connection implements an optional networked string table.
/// Users can then notify the connection of strings it references often, such as player names,
/// and transmit only a tag, instead of the whole string.
///
class connection : public ref_object
{
	friend class interface;
	/// Constants controlling the data representation of each packet header
	enum connection_constants {
		// NOTE - IMPORTANT!
		// The first bytes of each packet are made up of:
		// 1 bit - game data packet flag
		// 2 bits - packet type
		// sequence_number_bit_size bits - sequence number
		// ack_sequence_number_bit_size bits - high ack sequence received
		// these values should be set to align to a byte boundary, otherwise
		// bits will just be wasted.
		
		max_packet_window_size_shift = 5,                            ///< Packet window size is 2^max_packet_window_size_shift.
		max_packet_window_size = (1 << max_packet_window_size_shift),   ///< Maximum number of packets in the packet window.
		packet_window_mask = max_packet_window_size - 1,              ///< Mask for accessing the packet window.
		max_ack_mask_size = 1 << (max_packet_window_size_shift - 5),    ///< Each ack word can ack 32 packets.
		max_ack_byte_count = max_ack_mask_size << 2,                   ///< The maximum number of ack bytes sent in each packet.
		sequence_number_bit_size = 11,                              ///< Bit size of the send and sequence number.
		sequence_number_window_size = (1 << sequence_number_bit_size), ///< Size of the send sequence number window.
		sequence_number_mask = -sequence_number_window_size,          ///< Mask used to reconstruct the full send sequence number of the packet from the partial sequence number sent.
		ack_sequence_number_bit_size = 10,                           ///< Bit size of the ack receive sequence number.
		ack_sequence_number_window_size = (1 << ack_sequence_number_bit_size), ///< Size of the ack receive sequence number window.
		ack_sequence_number_mask = -ack_sequence_number_window_size,          ///< Mask used to reconstruct the full ack receive sequence number of the packet from the partial sequence number sent.
		
		packet_header_bit_size = 3 + ack_sequence_number_bit_size + sequence_number_bit_size, ///< Size, in bits, of the packet header sequence number section
		packet_header_byte_size = (packet_header_bit_size + 7) >> 3, ///< Size, in bytes, of the packet header sequence number information
		packet_header_pad_bits = (packet_header_byte_size << 3) - packet_header_bit_size, ///< Padding bits to get header bytes to align on a byte boundary, for encryption purposes.
		
		message_signature_bytes = 5, ///< Special data bytes written into the end of the packet to guarantee data consistency
	};
	uint32 _last_seq_recvd_at_send[max_packet_window_size]; ///< The sequence number of the last packet received from the remote host when we sent the packet with sequence X & packet_window_mask.
	uint32 _last_seq_recvd;                            ///< The sequence number of the most recently received packet from the remote host.
	uint32 _highest_acked_seq;                         ///< The highest sequence number the remote side has acknowledged.
	uint32 _last_send_seq;                             ///< The sequence number of the last packet sent.
	uint32 _ack_mask[max_ack_mask_size];                 ///< long string of bits, each acking a packet sent by the remote host.
	///< The bit associated with _last_seq_recvd is the low bit of the 0'th word of _ack_mask.
	uint32 _last_recv_ack_ack; ///< The highest sequence this side knows the other side has received an ACK or NACK for.
	
	uint32 _initial_send_seq; ///< The first _last_send_seq for this side of the connection.
	uint32 _initial_recv_seq; ///< The first _last_seq_recvd (the first _last_send_seq for the remote host).
	time _highest_acked_send_time; ///< The send time of the highest packet sequence acked by the remote host.  Used in the computation of round trip time.
	/// Two-bit identifier for each connected packet.
	enum net_packet_type
	{
		data_packet, ///< Standard data packet.  Each data packet sent increments the current packet sequence number (_last_send_seq).
		ping_packet, ///< Ping packet, sent if this instance hasn't heard from the remote host for a while.  Sending a
		///  ping packet does not increment the packet sequence number.
		ack_packet,  ///< Packet sent in response to a ping packet.  Sending an ack packet does not increment the sequence number.
		invalid_packet_type,
	};
	/// Constants controlling the behavior of pings and timeouts
	enum default_ping_constants {
		default_ping_timeout = 5000,  ///< Default milliseconds to wait before sending a ping packet.
		default_ping_retry_count = 5, ///< Default number of unacknowledged pings to send before timing out.
	};
	time _ping_timeout; ///< time to wait before sending a ping packet.
	uint32 _ping_retry_count; ///< Number of unacknowledged pings to send before timing out.
	
	/// Returns true if this connection has sent packets that have not yet been acked by the remote host.
	bool has_unacked_sent_packets() { return _last_send_seq != _highest_acked_seq; }
	
	byte_buffer_ptr _packet_data;
public:
	connection(random_generator &random_gen)
	{
		_initial_send_seq = random_gen.random_integer();
		random_gen.random_buffer((uint8 *) &_connection_parameters._nonce, sizeof(nonce));
		
		_simulated_latency = 0;
		_simulated_packet_loss = 0;
		
		_last_ping_send_time = time(0);
		_connection_state = not_connected;
		
		_ping_send_count = 0;
		
		_last_seq_recvd = 0;
		_highest_acked_seq = _initial_send_seq;
		_last_send_seq = _initial_send_seq; // start sending at _initial_send_seq + 1
		_ack_mask[0] = 0;
		_last_recv_ack_ack = 0;
		
		_ping_timeout = time(default_ping_timeout);
		_ping_retry_count = default_ping_retry_count;
	}
	
protected:
	/// Called when a pending connection is terminated
	virtual void on_connect_terminated(interface::termination_reason reason, byte_buffer_ptr &reject_buffer)
	{
		on_connection_terminated(reason, reject_buffer);
	}
	
	/// Called when this established connection is terminated for any reason
	virtual void on_connection_terminated(interface::termination_reason reason, byte_buffer_ptr &reason_buffer)
	{
		torque_socket_event event;
		if(reason != interface::reason_timed_out)
			event.event_type = torque_connection_disconnected_event_type;
		else
			event.event_type = torque_connection_timed_out_event_type;

		event.data_size = reason_buffer->get_buffer_size();
		memcpy(event.data, reason_buffer->get_buffer(), event.data_size);

		get_interface()->tnp_post_event(event, this);
	}
	
	/// Called when the connection is successfully established with the remote host.
	virtual void on_connection_established()
	{
		torque_socket_event event;
		event.event_type = torque_connection_established_event_type;
		get_interface()->tnp_post_event(event, this);
	}
	
	/// Validates that the given public key is valid for this connection.  If this
	/// host requires a valid certificate for the communication, this function
	/// should always return false.  It will only be called if the remote side
	/// of the connection did not provide a certificate.
	virtual bool validate_public_key(asymmetric_key *the_key, bool is_initiator) { return true; }
	
	/// Fills the connect request packet with additional custom data (from a subclass).
	virtual void write_connect_request(bit_stream &stream)
	{
		core::write(stream, _connection_parameters.connect_data);
	}
	
	/// Called after this connection instance is created on a non-initiating host (server).
	///
	/// Reads data sent by the write_connect_request method and returns true if the connection is accepted
	/// or false if it's not.  The error_string pointer should be filled if the connection is rejected.
	virtual bool read_connect_request(bit_stream &stream, byte_buffer_ptr &reason_buf)
	{
		core::read(stream, reason_buf);
		return true;
	}
	
	/// Writes any data needed to start the connection on the accept packet.
	virtual void write_connect_accept(bit_stream &stream)
	{
		core::write(stream, _connection_parameters.connect_data);;
	}
	
	/// Reads out the extra data read by write_connect_accept and returns true if it is processed properly.
	virtual bool read_connect_accept(bit_stream &stream, byte_buffer_ptr &error_buffer)
	{
		core::read(stream, error_buffer);
		return true;
	}
public:
	/// Returns the next send sequence that will be sent by this side.
	uint32 get_next_send_sequence() { return _last_send_seq + 1; }
	
	/// Returns the sequence of the last packet sent by this connection, or
	/// the current packet's send sequence if called from within write_packet().
	uint32 get_last_send_sequence() { return _last_send_seq; }
	
protected:
	/// Reads a raw packet from a bit_stream, as dispatched from interface.
	void read_raw_packet(bit_stream &bstream)
	{
		if(_simulated_packet_loss && _interface->random().random_unit_float() < _simulated_packet_loss)
		{
			TorqueLogMessageFormatted(LogNetConnection, ("connection %s: RECVDROP - %d", _address.to_string().c_str(), get_last_send_sequence()));
			return;
		}
		TorqueLogMessageFormatted(LogNetConnection, ("connection %s: RECV bytes", _address.to_string().c_str()));
		
		if(read_packet_header(bstream))
		{
			read_packet(bstream);
		}
	}
	
	virtual void read_packet(bit_stream &bstream)
	{
		byte_buffer_ptr data;
		core::read(bstream, data);

		torque_socket_event event;
		event.event_type = torque_connection_packet_event_type;
		event.packet_sequence = _last_seq_recvd;
		event.data_size = data->get_buffer_size();
		memcpy(event.data, data->get_buffer(), event.data_size);

		get_interface()->tnp_post_event(event, this);
	}
	
	
	/// Writes a full packet of the specified type into the bit_stream
	void write_raw_packet(bit_stream &bstream, net_packet_type packet_type)
	{
		write_packet_header(bstream, packet_type);
		if(packet_type == data_packet)
		{
			int32 start = bstream.get_bit_position();
			TorqueLogMessageFormatted(LogNetConnection, ("connection %s: START", _address.to_string().c_str()) );
			write_packet(bstream);
			TorqueLogMessageFormatted(LogNetConnection, ("connection %s: END - %llu bits", _address.to_string().c_str(), bstream.get_bit_position() - start) );
		}
		if(!_symmetric_cipher.is_null())
		{
			_symmetric_cipher->setup_counter(_last_send_seq, _last_seq_recvd, packet_type, 0);
			bit_stream_hash_and_encrypt(bstream, message_signature_bytes, packet_header_byte_size, _symmetric_cipher);
		}
	}
	
	virtual void write_packet(bit_stream &stream)
	{
		core::write(stream, _packet_data);
	}
	
	/// Writes the notify protocol's packet header into the bit_stream.
	void write_packet_header(bit_stream &stream, net_packet_type packet_type)
	{
		assert(!window_full() || packet_type != data_packet);
		
		int32 ack_byte_count = ((_last_seq_recvd - _last_recv_ack_ack + 7) >> 3);
		assert(ack_byte_count <= max_ack_byte_count);
		
		if(packet_type == data_packet)
			_last_send_seq++;
		
		stream.write_integer(packet_type, 2);
		stream.write_integer(_last_send_seq, 5); // write the first 5 bits of the send sequence
		stream.write_bool(true); // high bit of first byte indicates this is a data packet.
		stream.write_integer(_last_send_seq >> 5, sequence_number_bit_size - 5); // write the rest of the send sequence
		stream.write_integer(_last_seq_recvd, ack_sequence_number_bit_size);
		stream.write_integer(0, packet_header_pad_bits);
		
		stream.write_ranged_uint32(ack_byte_count, 0, max_ack_byte_count);
		
		uint32 word_count = (ack_byte_count + 3) >> 2;
		
		for(uint32 i = 0; i < word_count; i++)
			stream.write_integer(_ack_mask[i], i == word_count - 1 ?
								  (ack_byte_count - (i * 4)) * 8 : 32);
		
		// if we're resending this header, we can't advance the
		// sequence recieved (in case this packet drops and the prev one
		// goes through) 
		
		if(packet_type == data_packet)
			_last_seq_recvd_at_send[_last_send_seq & packet_window_mask] = _last_seq_recvd;
		
		//if(is_network_connection())
		//{
		//   TorqueLogMessageFormatted(LogBlah, ("SND: mLSQ: %08x  pkLS: %08x  pt: %d abc: %d",
		//      _last_send_seq, _last_seq_recvd, packet_type, ack_byte_count));
		//}
		
		TorqueLogMessageFormatted(LogConnectionProtocol, ("build hdr %d %d", _last_send_seq, packet_type));
	}
	
	/// Reads a notify protocol packet header from the bit_stream and
	/// returns true if it was a data packet that needs more processing.
	bool read_packet_header(bit_stream &pstream)
	{
		// read in the packet header:
		//
		//   2 bits packet type
		//   low 5 bits of the packet sequence number
		//   1 bit game packet
		//   sequence_number_bit_size-5 bits (packet seq number >> 5)
		//   ack_sequence_number_bit_size bits ackstart seq number
		//   packet_header_pad_bits = 0 - padding to byte boundary
		//   after this point, if this is an encrypted packet, all the rest of the data will be encrypted
		
		//   rangedU32 - 0...max_ack_byte_count
		//
		// type is:
		//    00 data packet
		//    01 ping packet
		//    02 ack packet
		
		// next 0...ack_byte_count bytes are ack flags
		//
		// return value is true if this is a valid data packet
		// or false if there is nothing more that should be read
		
		uint32 pk_packet_type     = pstream.read_integer(2);
		uint32 pk_sequence_number = pstream.read_integer(5);
		bool pk_data_packet_flag = pstream.read_bool();
		pk_sequence_number = pk_sequence_number | (pstream.read_integer(sequence_number_bit_size - 5) << 5);
		
		uint32 pk_highest_ack     = pstream.read_integer(ack_sequence_number_bit_size);
		uint32 pk_pad_bits        = pstream.read_integer(packet_header_pad_bits);
		
		if(pk_pad_bits != 0)
			return false;
		
		assert(pk_data_packet_flag);
		
		// verify packet ordering and acking and stuff
		// check if the 9-bit sequence is within the packet window
		// (within 31 packets of the last received sequence number).
		
		pk_sequence_number |= (_last_seq_recvd & sequence_number_mask);
		// account for wrap around
		if(pk_sequence_number < _last_seq_recvd)
			pk_sequence_number += sequence_number_window_size;
		
		// in the following test, account for wrap around from 0
		if(pk_sequence_number - _last_seq_recvd > (max_packet_window_size - 1))
		{
			// the sequence number is outside the window... must be out of order
			// discard.
			return false;
		}
		
		pk_highest_ack |= (_highest_acked_seq & ack_sequence_number_mask);
		// account for wrap around
		
		if(pk_highest_ack < _highest_acked_seq)
			pk_highest_ack += ack_sequence_number_window_size;
		
		if(pk_highest_ack > _last_send_seq)
		{
			// the ack number is outside the window... must be an out of order
			// packet, discard.
			return false;
		}
		
		if(!_symmetric_cipher.is_null())
		{
			_symmetric_cipher->setup_counter(pk_sequence_number, pk_highest_ack, pk_packet_type, 0);
			if(!bit_stream_decrypt_and_check_hash(pstream, message_signature_bytes, packet_header_byte_size, _symmetric_cipher))
			{
				TorqueLogMessage(LogNetConnection, ("Packet failed crypto"));
				return false;
			}
		}
		
		uint32 pk_ack_byte_count = pstream.read_ranged_uint32(0, max_ack_byte_count);
		if(pk_ack_byte_count > max_ack_byte_count || pk_packet_type >= invalid_packet_type)
			return false;
		
		uint32 pk_ack_mask[max_ack_mask_size];
		uint32 pk_ack_word_count = (pk_ack_byte_count + 3) >> 2;
		
		for(uint32 i = 0; i < pk_ack_word_count; i++)
			pk_ack_mask[i] = pstream.read_integer(i == pk_ack_word_count - 1 ? (pk_ack_byte_count - (i * 4)) * 8 : 32);
		
		//if(is_network_connection())
		//{
		//   TorqueLogMessageFormatted(LogBlah, ("RCV: mHA: %08x  pkHA: %08x  mLSQ: %08x  pkSN: %08x  pkLS: %08x  pkAM: %08x",
		//      _highest_acked_seq, pk_highest_ack, _last_send_seq, pk_sequence_number, _last_seq_recvd, pk_ack_mask[0]));
		//}
		
		static const char *packet_type_names[] = 
		{
			"data_packet",
			"ping_packet",
			"ack_packet",
		};
		
		TorqueLogBlock(LogConnectionProtocol,
					   for(uint32 i = _last_seq_recvd+1; i < pk_sequence_number; i++)
					   logprintf ("Not recv %d", i);
					   logprintf("Recv %d %s", pk_sequence_number, packet_type_names[pk_packet_type]);
					   );
		
		// shift up the ack mask by the packet difference
		// this essentially nacks all the packets dropped
		
		uint32 ack_mask_shift = pk_sequence_number - _last_seq_recvd;
		
		// if we've missed more than a full word of packets, shift up by words
		while(ack_mask_shift > 32)
		{
			for(int32 i = max_ack_mask_size - 1; i > 0; i--)
				_ack_mask[i] = _ack_mask[i-1];
			_ack_mask[0] = 0;
			ack_mask_shift -= 32;
		}
		
		// the first word upshifts all NACKs, except for the low bit, which is a
		// 1 if this is a data packet (i.e. not a ping packet or an ack packet)
		uint32 up_shifted = (pk_packet_type == data_packet) ? 1 : 0; 
		
		for(uint32 i = 0; i < max_ack_mask_size; i++)
		{
			uint32 next_shift = _ack_mask[i] >> (32 - ack_mask_shift);
			_ack_mask[i] = (_ack_mask[i] << ack_mask_shift) | up_shifted;
			up_shifted = next_shift;
		}
		
		// do all the notifies...
		uint32 notify_count = pk_highest_ack - _highest_acked_seq;
		for(uint32 i = 0; i < notify_count; i++) 
		{
			uint32 notify_index = _highest_acked_seq + i + 1;
			
			uint32 ack_mask_bit = (pk_highest_ack - notify_index) & 0x1F;
			uint32 ack_mask_word = (pk_highest_ack - notify_index) >> 5;
			
			bool packet_transmit_success = (pk_ack_mask[ack_mask_word] & (1 << ack_mask_bit)) != 0;
			TorqueLogMessageFormatted(LogConnectionProtocol, ("Ack %d %d", notify_index, packet_transmit_success));
			
			handle_notify(notify_index, packet_transmit_success);
			
			if(packet_transmit_success)
				_last_recv_ack_ack = _last_seq_recvd_at_send[notify_index & packet_window_mask];
		}
		// the other side knows more about its window than we do.
		if(pk_sequence_number - _last_recv_ack_ack > max_packet_window_size)
			_last_recv_ack_ack = pk_sequence_number - max_packet_window_size;
		
		_highest_acked_seq = pk_highest_ack;
		
		// first things first...
		// ackback any pings or half-full windows
		
		keep_alive(); // notification that the connection is ok
		
		uint32 prev_last_sequence = _last_seq_recvd;
		_last_seq_recvd = pk_sequence_number;
		
		if(pk_packet_type == ping_packet || (pk_sequence_number - _last_recv_ack_ack > (max_packet_window_size >> 1)))
		{
			// send an ack to the other side
			// the ack will have the same packet sequence as our last sent packet
			// if the last packet we sent was the connection accepted packet
			// we must resend that packet
			send_ack_packet();
		}
		return prev_last_sequence != pk_sequence_number && pk_packet_type == data_packet;
	}
	
	/// Sends a ping packet to the remote host, to determine if it is still alive and what its packet window status is.
	void send_ping_packet()
	{
		packet_stream ps;
		write_raw_packet(ps, ping_packet);
		TorqueLogMessageFormatted(LogConnectionProtocol, ("send ping %d", _last_send_seq));
		
		send_packet(ps);
	}
	
	/// Sends an ack packet to the remote host, in response to receiving a ping packet.
	void send_ack_packet()
	{
		packet_stream ps;
		write_raw_packet(ps, ack_packet);
		TorqueLogMessageFormatted(LogConnectionProtocol, ("send ack %d", _last_send_seq));
		
		send_packet(ps);
	}
	
	/// Dispatches a notify when a packet is ACK'd or NACK'd.
	virtual void handle_notify(uint32 sequence, bool recvd)
	{
		TorqueLogMessageFormatted(LogNetConnection, ("connection %s: NOTIFY %d %s", _address.to_string().c_str(), sequence, recvd ? "RECVD" : "DROPPED"));

		torque_socket_event event;
		event.event_type = torque_connection_packet_notify_event_type;
		event.delivered = recvd;
		event.packet_sequence = sequence;
		get_interface()->tnp_post_event(event, this);
	}
	
	/// Called when a packet is received to stop any timeout action in progress.
	void keep_alive()
	{
		_last_ping_send_time = time(0);
		_ping_send_count = 0;
	}
	
public:
	/// Sets the initial sequence number of packets read from the remote host.
	void set_initial_recv_sequence(uint32 sequence)
	{ 
		_initial_recv_seq = _last_seq_recvd = _last_recv_ack_ack = sequence;
	}
	
	/// Returns the initial sequence number of packets sent from the remote host.
	uint32 get_initial_recv_sequence() { return _initial_recv_seq; }
	
	/// Returns the initial sequence number of packets sent to the remote host.
	uint32 get_initial_send_sequence() { return _initial_send_seq; }
	
	/// Connect to a server through a given network interface.
	void connect(interface *connection_interface, const address &address, const byte_buffer_ptr& data)
	{
		_connection_parameters._is_initiator = true;
		_connection_parameters.connect_data = data;
		
		set_address(address);
		set_interface(connection_interface);
		_interface->start_connection(this);
	}
		
	/// Connects to a remote host that is also connecting to this connection (negotiated by a third party)
	void connect_arranged(interface *connection_interface, const array<address> &possible_addresses, nonce &my_nonce, nonce &remote_nonce, byte_buffer_ptr shared_secret, bool is_initiator)
	{
		_connection_parameters._possible_addresses = possible_addresses;
		_connection_parameters._is_initiator = is_initiator;
		_connection_parameters._is_arranged = true;
		_connection_parameters._nonce = my_nonce;
		_connection_parameters._server_nonce = remote_nonce;
		_connection_parameters._arranged_secret = shared_secret;
		
		set_interface(connection_interface);
		_interface->start_arranged_connection(this);   
	}
	
	/// Sends a disconnect packet to notify the remote host that this side is terminating the connection for the specified reason.
	/// This will remove the connection from its interface, and may have the side
	/// effect that the connection is deleted, if there are no other objects with RefPtrs
	/// to the connection.
	void disconnect(byte_buffer_ptr &reason)
	{
		_interface->disconnect(this, interface::reason_self_disconnect, reason);
	}
	
	/// Returns true if the packet send window is full and no more data packets can be sent.
	bool window_full()
	{
		if(_last_send_seq - _highest_acked_seq >= (max_packet_window_size - 2))
			return true;
		return false;
	}

	//----------------------------------------------------------------
	// Connection functions
	//----------------------------------------------------------------
	
private:
	time _last_update_time; ///< The last time a packet was sent from this instance.
	uint32 _simulated_latency; ///< Amount of additional time this connection delays its packet sends to simulate latency in the connection
	float32 _simulated_packet_loss; ///< Function to simulate packet loss on a network
	
	address _address; ///< The network address of the host this instance is connected to.
	
	// timeout management stuff:
	uint32 _ping_send_count; ///< Number of unacknowledged ping packets sent to the remote host
	time _last_ping_send_time; ///< Last time a ping packet was sent from this connection
	
protected:
	connection_parameters _connection_parameters;
public:
	connection_parameters &get_connection_parameters() { return _connection_parameters; }
	
	/// returns true if this ref_object initiated the connection with the remote host
	bool is_initiator() { return _connection_parameters._is_initiator; }
	
	uint32 _connect_send_count;    ///< Number of challenge or connect requests sent to the remote host.
	time _connect_last_send_time; ///< The send time of the last challenge or connect request.
	
protected:
	safe_ptr<interface> _interface;             ///< The interface of which this connection is a member.
public:
	/// Sets the interface this connection will communicate through.
    void set_interface(interface *my_interface)
	{
		_interface = my_interface;
	}
	
	/// Returns the interface this connection communicates through.
	interface *get_interface()
	{
		return _interface;
	}
	
protected:
	/// The helper ref_object that performs symmetric encryption on packets
	ref_ptr<symmetric_cipher> _symmetric_cipher;
public:
	/// Sets the symmetric_cipher this connection will use for encryption
	void set_symmetric_cipher(symmetric_cipher *the_cipher)
	{
		_symmetric_cipher = the_cipher;
	}
	
public:
	/// Sets the ping/timeout characteristics for a fixed-rate connection.  Total timeout is msPerPing * ping_retry_count.
	void set_ping_timeouts(time time_per_ping, uint32 ping_retry_count)
	{ _ping_retry_count = ping_retry_count; _ping_timeout = time_per_ping; }
	
	/// Simulates a network situation with a percentage random packet loss and a connection one way latency as specified.
	void set_simulated_net_params(float32 packet_loss, uint32 latency)
	{ _simulated_packet_loss = packet_loss; _simulated_latency = latency; }
	
	/// Returns the remote address of the host we're connected or trying to connect to.
	const address &get_address()
	{
		return _address;
	}
	
	/// Sets the address of the remote host we want to connect to.
	void set_address(const address &the_address)
	{
		_address = the_address;
	}
	
	/// Sends a packet that was written into a bit_stream to the remote host, or the _remote_connection on this host.
	udp_socket::send_to_result send_packet(bit_stream &stream)
	{
		if(_simulated_packet_loss && _interface->random().random_unit_float() < _simulated_packet_loss)
		{
			TorqueLogMessageFormatted(LogNetConnection, ("connection %s: SENDDROP - %d", _address.to_string().c_str(), get_last_send_sequence()));
			return udp_socket::send_to_success;
		}
		
		TorqueLogMessageFormatted(LogNetConnection, ("connection %s: SEND - %d bytes", _address.to_string().c_str(), stream.get_next_byte_position()));
		
		if(_simulated_latency)
		{
			_interface->send_to_delayed(get_address(), stream, _simulated_latency);
			return udp_socket::send_to_success;
		}
		else
			return _interface->send_to(get_address(), stream);
	}
	
	std::pair<udp_socket::send_to_result, uint32> tnp_send_data_packet(const byte_buffer_ptr& data)
	{
		_packet_data = data;
		packet_stream ps;
		write_raw_packet(ps, data_packet);
		TorqueLogMessageFormatted(LogConnectionProtocol, ("send data %d", _last_send_seq));
		
		return std::make_pair(send_packet(ps), _last_send_seq);
	}
	
	/// Checks to see if the connection has timed out, possibly sending a ping packet to the remote host.  Returns true if the connection timed out.
	bool check_timeout(time current_time)
	{
		if(_last_ping_send_time.get_milliseconds() == 0)
			_last_ping_send_time = current_time;
		
		time timeout = _ping_timeout;
		uint32 timeout_count = _ping_retry_count;

		if((current_time - _last_ping_send_time) > timeout)
		{
			if(_ping_send_count >= timeout_count)
				return true;
			_last_ping_send_time = current_time;
			_ping_send_count++;
			send_ping_packet();
		}
		return false;
	}
	
	/// Connection state flags for a connection instance.
	enum connection_state {
		not_connected=0, ///< Initial state of a connection instance - not connected.
		awaiting_challenge_response, ///< We've sent a challenge request, awaiting the response.
		sending_punch_packets, ///< The state of a pending arranged connection when both sides haven't heard from the other yet
		computing_puzzle_solution, ///< We've received a challenge response, and are in the process of computing a solution to its puzzle.
		awaiting_connect_response, ///< We've received a challenge response and sent a connect request.
		connect_timed_out, ///< The connection timed out during the connection process.
		connect_rejected, ///< The connection was rejected.
		connected, ///< We've accepted a connect request, or we've received a connect response accept.
		disconnected, ///< The connection has been disconnected.
		timed_out, ///< The connection timed out.
		state_count,
	};
	
	connection_state _connection_state; ///< Current state of this connection.
	
	/// Sets the current connection state of this connection.
	void set_connection_state(connection_state state) { _connection_state = state; }
	
	/// Gets the current connection state of this connection.
	connection_state get_connection_state() { return _connection_state; }
	
	/// Returns true if the connection handshaking has completed successfully.
	bool is_established() { return _connection_state == connected; }
	
};