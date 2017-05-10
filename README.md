# Siamese
## Fast and Portable Streaming Forward Error Correction in C

Siamese is a simple, portable, fast library for Forward Error Correction.
From a stream of input data it generates redundant data that can be used to
recover the lost originals without using acknowledgements.

Siamese is a streaming erasure code designed for low to medium rate streams
under 2000 packets per RTT (~3MB/s on the Internet), and modest loss rates
like 20% or less.  It works on mobile phones and mac/linux/windows computers.

It might be the first software library to enable this.  Please let me know if
you find software that also does this so I can take a look at the code and
incorporate any improvements into this library.

For faster file transfer it is better to not use FEC and instead use
retransmission for the bulk of lost data.
For higher loss rates it is better to use a stronger FEC library like Wirehair.
But for more normal situations this is a very useful tool.


##### Target application: Writing a rUDP Protocol

* If you're writing your own reliable UDP protocol, this can save you a bunch
of time for the trickier parts of the code to write.

It can also generate selective acknowledgements and retransmitted data to be
useful as the core engine of a Hybrid ARQ transport protocol, and it exposes
its custom memory allocator to help implement outgoing data queues.


##### Target application: Sending a multi-part file quickly

* If you're trying to send a file with under 2000 pieces (e.g. <3 MB of data
on the Internet) as fast as possible, then you can use this library to generate
some arbitrary amount of recovery data faster than Wirehair.

Recovery speed is proportional to the amount of loss, so this is efficient for
channels where almost all the data is expected to arrive with < 20% loss rate.
For small losses it's faster than Wirehair but for larger losses it is slower.
By betting that the network has a low (but non-zero) loss rate, median transfer
time improves by using this method.


Example codec usage with error checking omitted:

~~~
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
~~~
		
There are more detailed examples in `unit_test.cpp`.


#### Comparisons

Siamese is fairly different from the other FEC codecs I've released.

All the other ones are block codes, meaning they take a block of input at a time.  Summary:

[cm256](https://github.com/catid/cm256) : GF(256) Cauchy Reed-Solomon block code.  Limited to 255 inputs or outputs.  Input data cannot change between outputs.  Recovery never fails.

[longhair](https://github.com/catid/longhair) : Binary(XOR-only) Cauchy Reed-Solomon block code.  Limited to 255 inputs or outputs.  Inputs must be a multiple of 8 bytes.  Input data cannot change between outputs.  Recovery never fails.

[wirehair](https://github.com/catid/wirehair) : Complex LDPC+HDPC block code.  Up to 64,000 inputs in a block.  Unlimited outputs.  Decoder takes about the same time regardless of number of losses, implying that one lost packet takes a long time to recover.  Input data cannot change between outputs.  Recovery can fail about 1% of the time.

Siamese : Artifically limited to 16,000 inputs.  Artificially limited to 256 outputs.  Inputs *can* change between outputs.  Decoder takes time proportional to the number of losses as O(N^2).

For small loss count it's faster than Wirehair, and past a certain point (~10% loss) encode+decode time is longer than Wirehair.

At lower data rates, Siamese uses a Cauchy Reed-Solomon code: Recovery never fails.
At higher data rates, Siamese switches to a new structured linear convolutional code: It fails to recover about 1% of the time.

Many of the parameters of the code are tunable to trade between performance and recovery rate.


#### Credits

This software was written entirely by myself ( Christopher A. Taylor mrcatid@gmail.com ). If you find it useful and would like to buy me a coffee, consider tipping.
