/*
    Copyright (c) 2017 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Siamese nor the names of its contributors may be
      used to endorse or promote products derived from this software without
      specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/

#include "SiameseEncoder.h"
#include "SiameseLogging.h"
#include "SiameseSerializers.h"

namespace siamese {

#ifdef SIAMESE_ENCODER_DUMP_VERBOSE
    static logging::Channel Logger("Encoder", logging::Level::Debug);
#else
    static logging::Channel Logger("Encoder", logging::Level::Silent);
#endif


//------------------------------------------------------------------------------
// EncoderPacketWindow

EncoderPacketWindow::EncoderPacketWindow()
{
    NextColumn  = 0;
    ColumnStart = 0;

    ClearWindow();
}

void EncoderPacketWindow::ClearWindow()
{
    FirstUnremovedElement = 0;
    Count                 = 0;
    LongestPacket         = 0;
    SumStartElement       = 0;
    SumEndElement         = 0;

    for (unsigned laneIndex = 0; laneIndex < kColumnLaneCount; ++laneIndex)
    {
        EncoderColumnLane& lane = Lanes[laneIndex];

        for (unsigned sumIndex = 0; sumIndex < kColumnSumCount; ++sumIndex)
        {
            lane.Sum[sumIndex].Bytes   = 0;
            lane.NextElement[sumIndex] = laneIndex;
        }
        lane.LongestPacket = 0;
    }
}

SiameseResult EncoderPacketWindow::Add(SiameseOriginalPacket& packet)
{
    if (EmergencyDisabled)
        return Siamese_Disabled;
    if (Count >= SIAMESE_MAX_PACKETS)
        return Siamese_MaxPacketsReached;

    const unsigned column         = NextColumn;
    const unsigned subwindowCount = (unsigned)Subwindows.size();
    unsigned element              = Count;

    // Assign packet number
    packet.PacketNum = column;

    // If there is not enough room for this new element:
    // Note: Adding a buffer of kColumnLaneCount to create space ahead for
    // snapshots as a subwindow is filled and we need to store its snapshot
    if (element + kColumnLaneCount >= subwindowCount * kSubwindowSize)
    {
        Subwindows.resize(subwindowCount + 1);
        Subwindows[subwindowCount] = TheAllocator->Construct<EncoderSubwindow>();
        if (!Subwindows[subwindowCount])
        {
            EmergencyDisabled = true;
            Logger.Error("WindowAdd.Construct OOM");
            SIAMESE_DEBUG_BREAK;
            return Siamese_Disabled;
        }
    }

    if (Count > 0)
        ++Count;
    else
    {
        // Start a new window:
        element = column % kColumnLaneCount;
        StartNewWindow(column);
    }

    // Initialize original packet with received data
    OriginalPacket* original = GetWindowElement(element);
    if (0 == original->Initialize(TheAllocator, packet))
    {
        EmergencyDisabled = true;
        Logger.Error("WindowAdd.Initialize OOM");
        SIAMESE_DEBUG_BREAK;
        return Siamese_Disabled;
    }
    SIAMESE_DEBUG_ASSERT(original->Column % kColumnLaneCount == element % kColumnLaneCount);

    // Roll next column to assign
    NextColumn = IncrementColumn1(NextColumn);

    // Update longest packet
    const unsigned originalBytes = original->Buffer.Bytes;
    const unsigned laneIndex     = column % kColumnLaneCount;
    EncoderColumnLane& lane      = Lanes[laneIndex];
    if (lane.LongestPacket < originalBytes)
        lane.LongestPacket = originalBytes;
    if (LongestPacket < originalBytes)
        LongestPacket = originalBytes;

    Stats->Counts[SiameseEncoderStats_OriginalCount]++;
    Stats->Counts[SiameseEncoderStats_OriginalBytes] += packet.DataBytes;

    return Siamese_Success;
}

void EncoderPacketWindow::StartNewWindow(unsigned column)
{
    // Maintain the invariant that element % 8 == column % 8 by skipping some
    const unsigned element = column % kColumnLaneCount;
    ColumnStart            = column - element;
    SIAMESE_DEBUG_ASSERT(column >= element && ColumnStart < kColumnPeriod);
    SumStartElement        = element;
    SumEndElement          = element;
    FirstUnremovedElement  = element;
    Count                  = element + 1;

    // Reset longest packet
    LongestPacket = 0;
    for (unsigned laneIndex = 0; laneIndex < kColumnLaneCount; ++laneIndex)
        Lanes[laneIndex].LongestPacket = 0;

    Logger.Info(">>> Starting a new window from column ", ColumnStart);
}

void EncoderPacketWindow::RemoveBefore(unsigned firstKeptColumn)
{
    if (EmergencyDisabled)
        return;

    // Convert column to element, handling wrap-around:
    const unsigned firstKeptElement = ColumnToElement(firstKeptColumn);

    // If the column is outside of the window:
    if (InvalidElement(firstKeptElement))
    {
        // If the element was before the window:
        if (IsColumnDeltaNegative(firstKeptElement))
            Logger.Info("Remove before column ", firstKeptColumn, " - Ignored before window");
        else
        {
            // Removed everything
            Count = 0;

            Logger.Info("Remove before column ", firstKeptColumn, " - Removed everything");
        }
    }
    else
    {
        Logger.Info("Remove before column ", firstKeptColumn, " element ", firstKeptElement);

        // Mark these elements for removal next time we generate output
        if (FirstUnremovedElement < firstKeptElement)
            FirstUnremovedElement = firstKeptElement;
    }
}

void EncoderPacketWindow::ResetSums(unsigned elementStart)
{
    // Recreate all the sums from scratch after this:
    for (unsigned laneIndex = 0; laneIndex < kColumnLaneCount; ++laneIndex)
    {
        // Calculate first element to accumulate for this lane
        const unsigned nextElement = GetNextLaneElement(elementStart, laneIndex);

        for (unsigned sumIndex = 0; sumIndex < kColumnSumCount; ++sumIndex)
        {
            Lanes[laneIndex].NextElement[sumIndex] = nextElement;
            Lanes[laneIndex].Sum[sumIndex].Bytes   = 0;
        }
    }

    SumStartElement = elementStart;
    SumEndElement   = elementStart;
    SumColumnStart  = ElementToColumn(elementStart);
    SumErasedCount  = 0;
}

void EncoderPacketWindow::RemoveElements()
{
    const unsigned firstKeptSubwindow  = FirstUnremovedElement / kSubwindowSize;
    const unsigned removedElementCount = firstKeptSubwindow * kSubwindowSize;
    SIAMESE_DEBUG_ASSERT(firstKeptSubwindow >= 1);
    SIAMESE_DEBUG_ASSERT(removedElementCount % kColumnLaneCount == 0);
    SIAMESE_DEBUG_ASSERT(removedElementCount <= FirstUnremovedElement);

    Logger.Info("******** Removing up to ", FirstUnremovedElement, " and startColumn=", ColumnStart);

    // If there are running sums:
    if (SumEndElement > SumStartElement)
    {
        // Roll up the sums past the removal point
        for (unsigned laneIndex = 0; laneIndex < kColumnLaneCount; ++laneIndex)
        {
            for (unsigned sumIndex = 0; sumIndex < kColumnSumCount; ++sumIndex)
            {
                GetSum(laneIndex, sumIndex, removedElementCount);

                SIAMESE_DEBUG_ASSERT(Lanes[laneIndex].NextElement[sumIndex] >= removedElementCount);
                Lanes[laneIndex].NextElement[sumIndex] -= removedElementCount;
            }
        }

        if (removedElementCount > SumStartElement)
            SumErasedCount += removedElementCount - SumStartElement;

        if (SumEndElement > removedElementCount)
            SumEndElement -= removedElementCount;
        else
            SumEndElement = 0;

        if (SumStartElement > removedElementCount)
            SumStartElement -= removedElementCount;
        else
            SumStartElement = 0;
    }

    // Shift kept subwindows to the front of the vector
    // Note: Removed entries get rotated to the end
    std::rotate(Subwindows.begin(), Subwindows.begin() + firstKeptSubwindow, Subwindows.end());

    // Update the count of elements in the window
    SIAMESE_DEBUG_ASSERT(Count >= removedElementCount);
    Count -= removedElementCount;

    // Roll up the ColumnStart member
    ColumnStart = ElementToColumn(removedElementCount);
    SIAMESE_DEBUG_ASSERT(ColumnStart == Subwindows[0]->Originals[0].Column);

    // Roll up the FirstUnremovedElement member
    SIAMESE_DEBUG_ASSERT(FirstUnremovedElement % kSubwindowSize == FirstUnremovedElement - removedElementCount);
    SIAMESE_DEBUG_ASSERT(FirstUnremovedElement >= removedElementCount);
    FirstUnremovedElement -= removedElementCount;

    // Determine the new longest packets
    unsigned longestPacket = 0;
    unsigned laneLongest[kColumnLaneCount] = { 0 };
    for (unsigned i = FirstUnremovedElement, count = Count; i < count; ++i)
    {
        OriginalPacket* original     = GetWindowElement(i);
        const unsigned originalBytes = original->Buffer.Bytes;
        if (longestPacket < originalBytes)
            longestPacket = originalBytes;
        SIAMESE_DEBUG_ASSERT(original->Column % kColumnLaneCount == i % kColumnLaneCount);
        const unsigned laneIndex = i % kColumnLaneCount;
        if (laneLongest[laneIndex] < originalBytes)
            laneLongest[laneIndex] = originalBytes;
    }

    // Update longest packet fields
    LongestPacket = longestPacket;
    for (unsigned laneIndex = 0; laneIndex < kColumnLaneCount; ++laneIndex)
        Lanes[laneIndex].LongestPacket = laneLongest[laneIndex];

    // If there are no running sums:
    if (SumEndElement <= SumStartElement)
        ResetSums(FirstUnremovedElement);
}

const GrowingAlignedDataBuffer* EncoderPacketWindow::GetSum(unsigned laneIndex, unsigned sumIndex, unsigned elementEnd)
{
    EncoderColumnLane& lane = Lanes[laneIndex];
    unsigned element = lane.NextElement[sumIndex];
    SIAMESE_DEBUG_ASSERT(element % kColumnLaneCount == laneIndex);
    SIAMESE_DEBUG_ASSERT(element < Count + kColumnLaneCount);

    if (element < elementEnd)
    {
        GrowingAlignedDataBuffer& sum = lane.Sum[sumIndex];

        // Grow this sum for this lane to fit new (larger) data if needed
        if (lane.LongestPacket > 0 &&
            !sum.GrowZeroPadded(TheAllocator, lane.LongestPacket))
        {
            EmergencyDisabled = true;
            goto ExitSum;
        }

        do
        {
            Logger.Info("Lane ", laneIndex, " sum ", sumIndex, " accumulating column: ", ColumnStart + element);

            OriginalPacket* original = GetWindowElement(element);
            const unsigned column    = original->Column;
            unsigned addBytes        = original->Buffer.Bytes;

            if (!sum.GrowZeroPadded(TheAllocator, addBytes))
            {
                EmergencyDisabled = true;
                goto ExitSum;
            }

            SIAMESE_DEBUG_ASSERT(original->Buffer.Bytes <= sum.Bytes || element < FirstUnremovedElement);

            // Sum += PacketData
            if (sumIndex == 0)
                gf256_add_mem(sum.Data, original->Buffer.Data, addBytes);
            else
            {
                // Sum += CX[2] * PacketData
                uint8_t CX = GetColumnValue(column);
                if (sumIndex == 2)
                    CX = gf256_sqr(CX);
                gf256_muladd_mem(sum.Data, CX, original->Buffer.Data, addBytes);
            }

            SIAMESE_DEBUG_ASSERT(original->Column % kColumnLaneCount == laneIndex);
            element += kColumnLaneCount;
        } while (element < elementEnd);

        // Store next element to accumulate
        lane.NextElement[sumIndex] = element;
    }

ExitSum:
    return &lane.Sum[sumIndex];
}


//------------------------------------------------------------------------------
// EncoderAcknowledgementState

bool EncoderAcknowledgementState::OnAcknowledgementData(const uint8_t* data, unsigned bytes)
{
    unsigned nextColumnExpected = 0;
    int headerBytes = DeserializeHeader_PacketNum(data, bytes, nextColumnExpected);
    if (headerBytes < 1)
    {
        SIAMESE_DEBUG_BREAK; // Invalid input
        return false;
    }
    data += headerBytes, bytes -= headerBytes;

    // Ignore duplicate data
    if (NextColumnExpected == nextColumnExpected &&
        Data && bytes == DataBytes &&
        0 == memcmp(data, Data, bytes))
    {
        return true;
    }

    NextColumnExpected = nextColumnExpected;

    // Remove data the given column
    TheWindow->RemoveBefore(NextColumnExpected);

    // Reset message decoder state
    Offset     = 0;
    LossColumn = NextColumnExpected;
    LossCount  = 0;
    DataBytes  = bytes;

    // If there are no loss ranges:
    if (bytes <= 0)
        return true;

    // Copy the new data into place with some padding at the end
    Data = TheAllocator->Reallocate(Data, bytes + kPaddingBytes,
        ReallocBehavior::Uninitialized);
    memcpy(Data, data, bytes);
    memset(Data + bytes, 0, kPaddingBytes); // Zero guard bytes

    // Returns false if decoding the first loss range fails
    return DecodeNextRange();
}

bool EncoderAcknowledgementState::DecodeNextRange()
{
    // If there is no more loss range data to process:
    if (Offset >= DataBytes)
        return false;

    // Decode loss range format:

    SIAMESE_DEBUG_ASSERT(Data && DataBytes >= 1);

    unsigned relativeStart, lossCountM1;
    const int lossRangeBytes = DeserializeHeader_NACKLossRange(
        Data + Offset, DataBytes + kPaddingBytes - Offset, relativeStart, lossCountM1);
    if (lossRangeBytes < 0)
        return false;

    Offset += lossRangeBytes;
    if (Offset > DataBytes)
    {
        SIAMESE_DEBUG_BREAK; // Invalid input
        // TBD: Disable codec here?
        return false;
    }

    // Move ahead the loss column
    LossColumn = AddColumns(LossColumn, relativeStart);
    LossCount  = lossCountM1 + 1;

    return true;
}

bool EncoderAcknowledgementState::GetNextLossColumn(unsigned& columnOut)
{
    if (LossCount <= 0)
    {
        // Note: LossColumn is used as the offset for the next loss range, so
        // we should increment it to one beyond the end of the current region
        // when we get to the end of the region.
        LossColumn = IncrementColumn1(LossColumn);

        if (!DecodeNextRange())
            return false;
    }

    columnOut = LossColumn;

    LossColumn = IncrementColumn1(LossColumn);
    --LossCount;

    return true;
}

void EncoderAcknowledgementState::RestartLossIterator()
{
    // Reset message decoder state
    Offset     = 0;
    LossColumn = NextColumnExpected;
    LossCount  = 0;

    DecodeNextRange();
    // Note: Ignore return value
}

void EncoderAcknowledgementState::Clear()
{
    // Reset message decoder state
    Offset     = 0;
    LossColumn = 0;
    LossCount  = 0;
    DataBytes  = 0;
}


//------------------------------------------------------------------------------
// Encoder

Encoder::Encoder()
{
    Window.TheAllocator = &TheAllocator;
    Window.Stats        = &Stats;
    Ack.TheAllocator    = &TheAllocator;
    Ack.TheWindow       = &Window;
}

SiameseResult Encoder::Acknowledge(const uint8_t* data, unsigned bytes)
{
    if (Window.EmergencyDisabled)
        return Siamese_Disabled;

    if (!Ack.OnAcknowledgementData(data, bytes))
        return Siamese_InvalidInput;

    Stats.Counts[SiameseEncoderStats_AckCount]++;
    Stats.Counts[SiameseEncoderStats_AckBytes] += bytes;

    return Siamese_Success;
}

SiameseResult Encoder::Retransmit(unsigned retransmitMsec, SiameseOriginalPacket& originalOut)
{
    originalOut.Data      = nullptr;
    originalOut.DataBytes = 0;

    if (Window.EmergencyDisabled)
        return Siamese_Disabled;

    if (!Ack.HasNegativeAcknowledgements())
        return Siamese_NeedMoreData;

    const uint64_t nowMsec = siamese::GetTimeMsec();

    std::ostringstream* pDebugMsg = nullptr;
    if (Logger.ShouldLog(logging::Level::Debug))
    {
        delete pDebugMsg;
        pDebugMsg = new std::ostringstream();
        *pDebugMsg << "Encoder NACK parsing: Columns resent recently = {";
    }

    // While there is another loss column to process:
    while (Ack.GetNextLossColumn(originalOut.PacketNum))
    {
        // Note: This also works when Count == 0
        SIAMESE_DEBUG_ASSERT(originalOut.PacketNum >= Window.ColumnStart);
        const unsigned element = Window.ColumnToElement(originalOut.PacketNum);
        if (Window.InvalidElement(element))
        {
            SIAMESE_DEBUG_BREAK; // Should never happen
            break;
        }

        // Return the packet data
        OriginalPacket* original = Window.GetWindowElement(element);
        if (original->Buffer.Bytes <= 0)
        {
            SIAMESE_DEBUG_BREAK; // Should never happen
            break;
        }

        // If the packet cannot be resent yet:
        const uint64_t lastSendMsec = original->LastSendMsec;
        const uint64_t deltaMsec    = nowMsec - lastSendMsec;
        if (deltaMsec < retransmitMsec)
        {
            if (pDebugMsg)
                *pDebugMsg << " " << originalOut.PacketNum;
            continue;
        }

        if (pDebugMsg)
        {
            *pDebugMsg << " }. Found next column to retransmit: " << originalOut.PacketNum;
            Logger.Debug(pDebugMsg->str());
        }

        // Update last send time
        original->LastSendMsec = nowMsec;

        const unsigned headerBytes = original->HeaderBytes;
        SIAMESE_DEBUG_ASSERT(headerBytes > 0 && original->Buffer.Bytes > headerBytes);
        const unsigned length = original->Buffer.Bytes - headerBytes;

#ifdef SIAMESE_DEBUG
        // Check: Deserialize length from the front
        unsigned lengthCheck;
        int headerBytesCheck = DeserializeHeader_PacketLength(original->Buffer.Data, original->Buffer.Bytes, lengthCheck);
        if (lengthCheck != length || (int)headerBytes != headerBytesCheck ||
            headerBytesCheck < 1 || lengthCheck == 0 ||
            lengthCheck + headerBytesCheck != original->Buffer.Bytes)
        {
            SIAMESE_DEBUG_BREAK; // Invalid input
            Window.EmergencyDisabled = true;
            return Siamese_Disabled;
        }
#endif // SIAMESE_DEBUG

        originalOut.Data      = original->Buffer.Data + headerBytes;
        originalOut.DataBytes = length;

        Stats.Counts[SiameseEncoderStats_RetransmitCount]++;
        Stats.Counts[SiameseEncoderStats_RetransmitBytes] += length;

        return Siamese_Success;
    }

    // Restart the iterator through loss ranges after this
    Ack.RestartLossIterator();

    if (pDebugMsg)
    {
        *pDebugMsg << " }. Restarted loss range iterator";
        Logger.Debug(pDebugMsg->str());
    }

    return Siamese_NeedMoreData;
}

void Encoder::AddDenseColumns(unsigned row, uint8_t* productWorkspace)
{
    const unsigned recoveryBytes = Window.LongestPacket;

    // For each lane:
    for (unsigned laneIndex = 0; laneIndex < kColumnLaneCount; ++laneIndex)
    {
        // Compute the operations to run for this lane and row
        unsigned opcode = GetRowOpcode(laneIndex, row);

        // For summations into the RecoveryPacket buffer:
        unsigned mask = 1;
        for (unsigned sumIndex = 0; sumIndex < kColumnSumCount; ++sumIndex)
        {
            if (opcode & mask)
            {
                const GrowingAlignedDataBuffer* sum = Window.GetSum(laneIndex, sumIndex, Window.Count);
                unsigned addBytes = sum->Bytes;
                if (addBytes > 0)
                {
                    if (addBytes > recoveryBytes)
                        addBytes = recoveryBytes;
                    gf256_add_mem(RecoveryPacket.Data, sum->Data, addBytes);
                }
            }
            mask <<= 1;
        }

        // For summations into the ProductWorkspace buffer:
        for (unsigned sumIndex = 0; sumIndex < kColumnSumCount; ++sumIndex)
        {
            if (opcode & mask)
            {
                const GrowingAlignedDataBuffer* sum = Window.GetSum(laneIndex, sumIndex, Window.Count);
                unsigned addBytes = sum->Bytes;
                if (addBytes > 0)
                {
                    if (addBytes > recoveryBytes)
                        addBytes = recoveryBytes;
                    gf256_add_mem(productWorkspace, sum->Data, addBytes);
                }
            }
            mask <<= 1;
        }
    }

    // Keep track of where the sum ended
    Window.SumEndElement = Window.Count;
}

void Encoder::AddLightColumns(unsigned row, uint8_t* productWorkspace)
{
    const unsigned startElement = Window.FirstUnremovedElement;
    SIAMESE_DEBUG_ASSERT(Window.SumEndElement >= startElement);
    const unsigned count = Window.SumEndElement - startElement;
    SIAMESE_DEBUG_ASSERT(count >= 2);
    SIAMESE_DEBUG_ASSERT(count <= Window.Count);

    PCGRandom prng;
    prng.Seed(row, count);

    std::ostringstream* pDebugMsg = nullptr;
    if (Logger.ShouldLog(logging::Level::Debug))
    {
        delete pDebugMsg;
        pDebugMsg = new std::ostringstream();
        *pDebugMsg << "LDPC columns: ";
    }

    const unsigned pairCount = (count + kPairAddRate - 1) / kPairAddRate;
    for (unsigned i = 0; i < pairCount; ++i)
    {
        const unsigned element1          = startElement + (prng.Next() % count);
        const OriginalPacket* original1  = Window.GetWindowElement(element1);
        const unsigned elementRX         = startElement + (prng.Next() % count);
        const OriginalPacket* originalRX = Window.GetWindowElement(elementRX);

        if (pDebugMsg)
            *pDebugMsg << element1 << " " << elementRX << " ";

        SIAMESE_DEBUG_ASSERT(original1->Column  == Window.ColumnStart + element1);
        SIAMESE_DEBUG_ASSERT(originalRX->Column == Window.ColumnStart + elementRX);
        SIAMESE_DEBUG_ASSERT(Window.LongestPacket >= original1->Buffer.Bytes);
        SIAMESE_DEBUG_ASSERT(Window.LongestPacket >= originalRX->Buffer.Bytes);

        gf256_add_mem(RecoveryPacket.Data, original1->Buffer.Data,  original1->Buffer.Bytes);
        gf256_add_mem(productWorkspace,    originalRX->Buffer.Data, originalRX->Buffer.Bytes);
    }

    if (pDebugMsg)
        Logger.Debug(pDebugMsg->str());
}

SiameseResult Encoder::Encode(SiameseRecoveryPacket& packet)
{
    if (Window.EmergencyDisabled)
        return Siamese_Disabled;

    // If there are no packets so far:
    if (Window.Count <= 0)
    {
        packet.DataBytes = 0;
        return Siamese_NeedMoreData;
    }

    // Get the number of packets in the window that are in flight (unacked)
    const unsigned unacknowledgedCount = Window.GetUnacknowledgedCount();

    // If there is only a single packet so far:
    if (unacknowledgedCount == 1)
        return GenerateSinglePacket(packet);

    // Calculate upper bound on width of sum for this recovery packet
    SIAMESE_DEBUG_ASSERT(Window.Count + Window.SumErasedCount >= Window.SumStartElement);
    const unsigned newSumCountUB = Window.Count - Window.SumStartElement + Window.SumErasedCount;

    // If sums should be reset because the range is empty or too large:
    if (Window.SumEndElement <= Window.SumStartElement ||
        newSumCountUB >= SIAMESE_MAX_PACKETS)
    {
#ifdef SIAMESE_ENABLE_CAUCHY
        // If the number of packets in flight is small enough, use Cauchy rows for now:
        if (unacknowledgedCount <= SIAMESE_CAUCHY_THRESHOLD)
            return GenerateCauchyPacket(packet);
#endif // SIAMESE_ENABLE_CAUCHY

        Logger.Debug("Resetting sums at element ", Window.FirstUnremovedElement);

        Window.ResetSums(Window.FirstUnremovedElement);
    }
#ifdef SIAMESE_ENABLE_CAUCHY
    else
    {
        // If the number of packets in flight may indicate Cauchy is better or we need to use it:
        if (unacknowledgedCount <= SIAMESE_SUM_RESET_THRESHOLD ||
            newSumCountUB <= SIAMESE_CAUCHY_THRESHOLD)
        {
            SIAMESE_DEBUG_ASSERT(newSumCountUB >= unacknowledgedCount);
            static_assert(SIAMESE_SUM_RESET_THRESHOLD <= SIAMESE_CAUCHY_THRESHOLD, "Update this too");

            // Stop using sums
            Window.SumEndElement = Window.SumStartElement;

            return GenerateCauchyPacket(packet);
        }
    }
#endif // SIAMESE_ENABLE_CAUCHY

    // Remove any data from the window at this point
    if (Window.FirstUnremovedElement >= kEncoderRemoveThreshold)
        Window.RemoveElements();

    // Advance row index
    const unsigned row = NextRow;
    if (++NextRow >= kRowPeriod)
        NextRow = 0;

    // Reset workspaces
    const unsigned recoveryBytes = Window.LongestPacket;
    const unsigned alignedBytes = NextAlignedOffset(recoveryBytes);
    if (!RecoveryPacket.Initialize(&TheAllocator, 2 * alignedBytes + kMaxRecoveryMetadataBytes))
    {
        Window.EmergencyDisabled = true;
        return Siamese_Disabled;
    }
    SIAMESE_DEBUG_ASSERT(RecoveryPacket.Bytes >= alignedBytes * 2);
    memset(RecoveryPacket.Data, 0, alignedBytes * 2);
    uint8_t* productWorkspace = RecoveryPacket.Data + alignedBytes;

    // Generate the recovery packet
    AddDenseColumns(row, productWorkspace);
    AddLightColumns(row, productWorkspace);

    // RecoveryPacket += RX * ProductWorkspace
    const uint8_t RX = GetRowValue(row);
    gf256_muladd_mem(RecoveryPacket.Data, RX, productWorkspace, recoveryBytes);

    RecoveryMetadata metadata;
    SIAMESE_DEBUG_ASSERT(Window.SumEndElement + Window.SumErasedCount >= Window.SumStartElement);
    metadata.SumCount = Window.SumEndElement - Window.SumStartElement + Window.SumErasedCount;
    metadata.LDPCCount   = unacknowledgedCount;
    metadata.ColumnStart = Window.SumColumnStart;
    metadata.Row         = row;

    // Serialize metadata into the last few bytes of the packet
    // Note: This saves an extra copy to move the data around
    const unsigned footerBytes = SerializeFooter_RecoveryMetadata(metadata, RecoveryPacket.Data + recoveryBytes);
    packet.Data      = RecoveryPacket.Data;
    packet.DataBytes = recoveryBytes + footerBytes;

    Stats.Counts[SiameseEncoderStats_RecoveryCount]++;
    Stats.Counts[SiameseEncoderStats_RecoveryBytes] += packet.DataBytes;

    Logger.Info("Generated Siamese sum recovery packet start=", metadata.ColumnStart, " ldpcCount=", metadata.LDPCCount, " sumCount=", metadata.SumCount, " row=", metadata.Row);

    return Siamese_Success;
}

SiameseResult Encoder::Get(SiameseOriginalPacket& packetOut)
{
    // Note: Keep this in sync with Decoder::Get

    if (Window.EmergencyDisabled)
        return Siamese_Disabled;

    // Note: This also works when Count == 0
    SIAMESE_DEBUG_ASSERT(packetOut.PacketNum >= Window.ColumnStart);
    const unsigned element = Window.ColumnToElement(packetOut.PacketNum);
    if (Window.InvalidElement(element))
    {
        // Set default return value
        packetOut.Data      = nullptr;
        packetOut.DataBytes = 0;
        return Siamese_NeedMoreData;
    }

    // Return the packet data
    OriginalPacket* original = Window.GetWindowElement(element);
    if (original->Buffer.Bytes <= 0)
    {
        packetOut.Data      = nullptr;
        packetOut.DataBytes = 0;
        return Siamese_NeedMoreData;
    }

    const unsigned headerBytes = original->HeaderBytes;
    SIAMESE_DEBUG_ASSERT(headerBytes > 0 && original->Buffer.Bytes > headerBytes);
    const unsigned length = original->Buffer.Bytes - headerBytes;

#ifdef SIAMESE_DEBUG
    // Check: Deserialize length from the front
    unsigned lengthCheck;
    int headerBytesCheck = DeserializeHeader_PacketLength(original->Buffer.Data, original->Buffer.Bytes, lengthCheck);
    if (lengthCheck != length || (int)headerBytes != headerBytesCheck ||
        headerBytesCheck < 1 || lengthCheck == 0 ||
        lengthCheck + headerBytesCheck != original->Buffer.Bytes)
    {
        SIAMESE_DEBUG_BREAK; // Invalid input
        Window.EmergencyDisabled = true;
        return Siamese_Disabled;
    }
#endif // SIAMESE_DEBUG

    packetOut.Data      = original->Buffer.Data + headerBytes;
    packetOut.DataBytes = length;
    return Siamese_Success;
}

SiameseResult Encoder::GenerateSinglePacket(SiameseRecoveryPacket& packet)
{
    OriginalPacket* original     = Window.GetWindowElement(Window.FirstUnremovedElement);
    const unsigned originalBytes = original->Buffer.Bytes;

    // Note: This often does not actually reallocate or move since we overallocate
    if (!original->Buffer.GrowZeroPadded(&TheAllocator, originalBytes + kMaxRecoveryMetadataBytes))
    {
        Window.EmergencyDisabled = true;
        return Siamese_Disabled;
    }

    // Set bytes back to original
    original->Buffer.Bytes = originalBytes;

    // Serialize metadata into the last few bytes of the packet
    // Note: This saves an extra copy to move the data around
    RecoveryMetadata metadata;
    metadata.SumCount    = 1;
    metadata.LDPCCount   = 1;
    metadata.ColumnStart = original->Column;
    metadata.Row         = 0;

    const unsigned footerBytes = SerializeFooter_RecoveryMetadata(metadata, original->Buffer.Data + originalBytes);
    packet.Data      = original->Buffer.Data;
    packet.DataBytes = originalBytes + footerBytes;

    Logger.Info("Generated single recovery packet start=", metadata.ColumnStart, " ldpcCount=", metadata.LDPCCount, " sumCount=", metadata.SumCount, " row=", metadata.Row);

    Stats.Counts[SiameseEncoderStats_RecoveryCount]++;
    Stats.Counts[SiameseEncoderStats_RecoveryBytes] += packet.DataBytes;

    return Siamese_Success;
}


#ifdef SIAMESE_ENABLE_CAUCHY

SiameseResult Encoder::GenerateCauchyPacket(SiameseRecoveryPacket& packet)
{
    // Reset recovery packet
    const unsigned firstElement  = Window.FirstUnremovedElement;
    const unsigned recoveryBytes = Window.LongestPacket;
    if (!RecoveryPacket.Initialize(&TheAllocator, recoveryBytes + kMaxRecoveryMetadataBytes))
    {
        Window.EmergencyDisabled = true;
        return Siamese_Disabled;
    }

    const unsigned unacknowledgedCount = Window.GetUnacknowledgedCount();
    RecoveryMetadata metadata;
    metadata.SumCount    = unacknowledgedCount;
    metadata.LDPCCount   = unacknowledgedCount;
    metadata.ColumnStart = Window.ElementToColumn(firstElement);

    // We have to recalculate the number of used bytes since the Cauchy/parity rows may be
    // shorter since they do not need to contain the start of the window which may be acked.
    unsigned usedBytes = 0;

    // If it is time to generate a new parity row:
    const unsigned nextParityElement = Window.ColumnToElement(NextParityColumn);
    if (nextParityElement <= firstElement || IsColumnDeltaNegative(nextParityElement))
    {
        // Set next time we write a parity row
        NextParityColumn = AddColumns(metadata.ColumnStart, unacknowledgedCount);

        // Row 0 is a parity row
        metadata.Row = 0;

        // Unroll first column
        OriginalPacket* original = Window.GetWindowElement(firstElement);
        unsigned originalBytes   = original->Buffer.Bytes;

        memcpy(RecoveryPacket.Data, original->Buffer.Data, originalBytes);
        // Pad the rest out with zeroes to avoid corruption
        SIAMESE_DEBUG_ASSERT(RecoveryPacket.Bytes >= originalBytes);
        memset(RecoveryPacket.Data + originalBytes, 0, recoveryBytes - originalBytes);

        usedBytes = originalBytes;

        // For each remaining column:
        for (unsigned element = firstElement + 1, count = Window.Count; element < count; ++element)
        {
            original      = Window.GetWindowElement(element);
            originalBytes = original->Buffer.Bytes;

            SIAMESE_DEBUG_ASSERT(RecoveryPacket.Bytes >= originalBytes);

            gf256_add_mem(RecoveryPacket.Data, original->Buffer.Data, originalBytes);

            if (usedBytes < originalBytes)
                usedBytes = originalBytes;
        }
    }
    else
    {
        // Select Cauchy row number
        const unsigned cauchyRow = NextCauchyRow;
        metadata.Row = cauchyRow + 1;
        if (++NextCauchyRow >= kCauchyMaxRows)
            NextCauchyRow = 0;

        // Unroll first column
        unsigned cauchyColumn    = metadata.ColumnStart % kCauchyMaxColumns;
        OriginalPacket* original = Window.GetWindowElement(firstElement);
        uint8_t y                = CauchyElement(cauchyRow, cauchyColumn);
        unsigned originalBytes   = original->Buffer.Bytes;

        gf256_mul_mem(RecoveryPacket.Data, original->Buffer.Data, y, originalBytes);
        // Pad the rest out with zeroes to avoid corruption
        SIAMESE_DEBUG_ASSERT(recoveryBytes >= originalBytes);
        SIAMESE_DEBUG_ASSERT(RecoveryPacket.Bytes >= originalBytes);
        memset(RecoveryPacket.Data + originalBytes, 0, recoveryBytes - originalBytes);

        usedBytes = originalBytes;

        // For each remaining column:
        for (unsigned element = firstElement + 1, count = Window.Count; element < count; ++element)
        {
            cauchyColumn  = (cauchyColumn + 1) % kCauchyMaxColumns;
            original      = Window.GetWindowElement(element);
            originalBytes = original->Buffer.Bytes;
            y             = CauchyElement(cauchyRow, cauchyColumn);

            SIAMESE_DEBUG_ASSERT(RecoveryPacket.Bytes >= originalBytes);

            gf256_muladd_mem(RecoveryPacket.Data, y, original->Buffer.Data, originalBytes);

            if (usedBytes < originalBytes)
                usedBytes = originalBytes;
        }
    }

    // Slap metadata footer on the end
    const unsigned footerBytes = SerializeFooter_RecoveryMetadata(metadata, RecoveryPacket.Data + usedBytes);

    packet.Data      = RecoveryPacket.Data;
    packet.DataBytes = usedBytes + footerBytes;

    Logger.Info("Generated Cauchy/parity recovery packet start=", metadata.ColumnStart, " ldpcCount=", metadata.LDPCCount, " sumCount=", metadata.SumCount, " row=", metadata.Row);

    Stats.Counts[SiameseEncoderStats_RecoveryCount]++;
    Stats.Counts[SiameseEncoderStats_RecoveryBytes] += packet.DataBytes;

    return Siamese_Success;
}

#endif // SIAMESE_ENABLE_CAUCHY

SiameseResult Encoder::GetStatistics(uint64_t* statsOut, unsigned statsCount)
{
    if (statsCount > SiameseEncoderStats_Count)
        statsCount = SiameseEncoderStats_Count;

    // Fill in memory allocated
    Stats.Counts[SiameseEncoderStats_MemoryUsed] = TheAllocator.GetMemoryAllocatedBytes();

    for (unsigned i = 0; i < statsCount; ++i)
        statsOut[i] = Stats.Counts[i];

    return Siamese_Success;
}


} // namespace siamese
