Siamese Streaming Forward Error Correction

Siamese is a simple, portable library for Forward Error Correction in C++.
From a stream of input data it generates redundant data that can be used to
recover the lost originals without using acknowledgements.

Siamese is a streaming erasure code designed for low to medium rate streams
under 2000 packets per RTT (~3MB/s on the Internet), and modest loss rates
like 20% or less.  It works on mobile phones and mac/linux/windows computers.

For faster file transfer it is better to not use FEC and instead use
retransmission for the bulk of lost data.
For higher loss rates it is better to use a stronger FEC library like Wirehair.
But for more normal situations this is a very useful tool.


Target application: Writing a rUDP Protocol

* If you're writing your own reliable UDP protocol, this can save you a bunch
of time for the trickier parts of the code to write.

It can also generate selective acknowledgements and retransmitted data to be
useful as the core engine of a Hybrid ARQ transport protocol, and it exposes
its custom memory allocator to help implement outgoing data queues.


Target application: Sending a multi-part file quickly

* If you're trying to send a file with under 2000 pieces (e.g. <3 MB of data
on the Internet) as fast as possible, then you can use this library to generate
some arbitrary amount of recovery data faster than Wirehair.

Recovery speed is proportional to the amount of loss, so this is efficient for
channels where almost all the data is expected to arrive with < 20% loss rate.
For small losses it's faster than Wirehair but for larger losses it is slower.
By betting that the network has a low (but non-zero) loss rate, median transfer
time improves by using this method.


Example codec usage with error checking omitted:

    SiameseEncoder encoder = siamese_encoder_create();
    SiameseDecoder decoder = siamese_decoder_create();

	// For each original datagram:
	
		SiameseOriginalPacket original;
		original.Data = buffer;
		original.DataBytes = bytes;

		siamese_encoder_add(encoder, &original);
		siamese_decoder_add_original(decoder, &original);

	// For each recovery datagram:

		SiameseRecoveryPacket recovery;
		siamese_encode(encoder, &recovery);

		siamese_decoder_add_recovery(decoder, &recovery);

		if (0 == siamese_decoder_is_ready(decoder))
		{
            SiameseOriginalPacket* recovered = nullptr;
			unsigned recoveredCount = 0;
			siamese_decode(decoder, &recovered, &recoveredCount);
			
			// Process recovered data here.
		}

		
There are more detailed examples in `unit_test.cpp`.