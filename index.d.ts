/// <reference types="node" />

import {EventEmitter} from "events";

export enum DataType{
    BOOLEAN,
    TINYINT,
    SMALLINT,
    INT,
    BIGINT,
    FLOAT,
    DOUBLE,
    STRING,
    BINARY,
    TIMESTAMP,
    ARRAY,
    MAP,
    STRUCT,
    UNION,
    DECIMAL,
    DATE,
    VARCHAR,
    CHAR
}

export namespace norc {
    export class Reader extends EventEmitter {
        readonly type: string
        readonly writeVersion: string
        readonly formatVersion: string
        readonly compressions: string

        constructor(input: string|Buffer)

        /**
         * Read all contents into memory and emit on Reader.on('data', (data:string) => void): void
         */
        read(): void
        /**
         * Read all contents into memory, if a callback is defined an iterator will be returned otherwise listen .on('data') for content chunks
         */
        read(cb:(err:Error, data: Iterator<object>|null) => void): void
        /**
         * Read only columns defined in opts.columns, get contents back as either an event(data:string) or an array iterator
         * The iterator will return the contents as an object, whereas the event will return a string which will subsequently need to be JSON parse(d).
         * The event is more performant, but the iterator offers more control.
         * @param opts
         * @param cb
         */
        read(opts: {resultType?: 'iterator'|'event', columns?: string[]}, cb?:(err:Error, data: Iterator<object>|null) => void): void

        columnStatistics(column:string): string
    }
    export type ORC_ROW = {[key: string]: string|boolean|number|null}
    export class Writer {
        constructor(output?: string)
        fromCsv(file: string, cb: (err: Error, norc: Writer) => void): void
        schema(v: {[key:string]: DataType}|string): void
        /**
         * Add a single entry (struct) to the file
         * @param row - struct
         */
        add(row: ORC_ROW): void
        /**
         * Add a collection of structs to the file.
         * This is the preferred method of adding data to a file.
         */
        add(rows: ORC_ROW[]): void
        /**
         * Close the file stream.
         */
        close(): void

        /**
         * This method is memory intensive as the conditional (if defined) will call back into the JS runtime to execute
         * the function and then use the results to determine if the row should be added to the merged file.
         * This is currently all done in a synchronous manner.
         * @param file
         * @param condition
         * @todo update to execute in seperate thread.
         */
        merge(file: string|Buffer, condition?: (r: ORC_ROW) => boolean): void
        /**
         * If the file is writing to a buffer, retrieve the buffer, must be called after a call to close
         */
        data(): Buffer
    }
}
