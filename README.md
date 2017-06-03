# Siamese
## Fast and Portable Streaming Erasure Correction Codes in C

Siamese is a fast and portable library for Erasure Correction.
From a real-time stream of input data it generates redundant data that can be used to
recover the lost originals without using acknowledgements.

Siamese is a streaming erasure code designed for low to medium rate streams
under 2000 packets per RTT (~3MB/s on the Internet), and modest loss rates
like 20% or less.  It works on mobile phones and mac/linux/windows computers.


##### What's unique about this approach?

This library implements a new type of convolutional code for erasure correction.
Since it is not a block code, as soon as recovery data arrives it can be used to recover original data.
The advantages this provides are explained thoroughly in this article:

[Block or Convolutional AL-FEC Codes? A Performance
Comparison for Robust Low-Latency Communications](https://hal.inria.fr/hal-01395937v2/document) [1].

~~~
[1] Vincent Roca, Belkacem Teibi, Christophe Burdinat, Tuan Tran-Thai, C´edric Thienot. Block
or Convolutional AL-FEC Codes? A Performance Comparison for Robust Low-Latency Communications.
2017. <hal-01395937v2>
~~~

For each recovery symbol that it produces, it stores and reuses some of the intermediate work, so that producing the next symbol takes much less time than usual.  As a side-effect, this approach only works for full (not partial) reliable data delivery.

There is also a block code (rather than streaming) implementation here: [fecal](https://github.com/catid/fecal).  It's easier to understand the new convolutional code math by reading through the encoder of that software and its readme.


##### Target application: Writing a rUDP Protocol

* If you're writing your own reliable UDP protocol, this can save you a bunch
of time for the trickier parts of the code to write.

It can also generate selective acknowledgements and retransmitted data to be
useful as the core engine of a Hybrid ARQ transport protocol, and it exposes
its custom memory allocator to help implement outgoing data queues.


##### Example Usage:

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
