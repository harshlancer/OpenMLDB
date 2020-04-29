package com._4paradigm.fesql.offline;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.Serializable;
import java.nio.ByteBuffer;


/**
 * By default, ByteBuffer instances are not serializable, tihs class implemented
 * serializable wrapper for byte buffer to communicate serialize buffer content.
 */
public class SerializableByteBuffer implements Serializable {

    transient private ByteBuffer buffer;

    static private final int MAGIC_END_TAG = 42;

    public SerializableByteBuffer() {}
    public SerializableByteBuffer(ByteBuffer buffer) { this.buffer = buffer; }

    public ByteBuffer getBuffer() {
        return buffer;
    }


    private void writeObject(java.io.ObjectOutputStream out) throws IOException {
        if (buffer == null) {
            throw new IOException("No backed buffer");
        }
        out.defaultWriteObject();
        out.writeInt(buffer.capacity());
        out.writeBoolean(buffer.isDirect());
        if (buffer.hasArray()) {
            out.write(buffer.array(), 0, buffer.capacity());
        } else {
            byte[] bytes = new byte[buffer.capacity()];
            ByteBuffer view = buffer.duplicate();
            view.rewind();
            view.get(bytes, 0, bytes.length);
            out.write(bytes);
        }
        out.writeInt(MAGIC_END_TAG);
    }


    private void readObject(java.io.ObjectInputStream in) throws IOException, ClassNotFoundException {
        in.defaultReadObject();

        // object stream is backed by block stream, thus read bytes
        // operations should be buffered to ensure exact bytes are read
        DataInputStream wrappedIStream = new DataInputStream(in);

        int capacity = wrappedIStream.readInt();
        boolean isDirect = wrappedIStream.readBoolean();
        if (isDirect) {
            buffer = ByteBuffer.allocateDirect(capacity);
        } else {
            buffer = ByteBuffer.allocate(capacity);
        }
        byte[] bytes;
        if (buffer.hasArray()) {
            bytes = buffer.array();
        } else {
            bytes = new byte[capacity];
        }

        try {
            wrappedIStream.readFully(bytes, 0, capacity);
        } catch (IOException e) {
            throw new IOException("Byte buffer stream corrupt, " +
                    "expect buffer bytes: " + capacity, e);
        }
        if (!buffer.hasArray()) {  // maybe direct
            buffer.put(bytes, 0, capacity);
            buffer.rewind();
        }
        int endTag = wrappedIStream.readInt();
        if (endTag != MAGIC_END_TAG) {
            throw new IOException("Byte buffer stream corrupt");
        }
    }
}
