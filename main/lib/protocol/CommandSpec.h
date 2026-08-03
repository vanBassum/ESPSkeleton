#pragma once

// Usage sketch only.
//
//   partition list
//   partition write -partition ota_1 -address 0x123456 \n <raw bytes…>
//   partition read  -partition ota_1 -address 0 -length 64 -ascii
//   help
//   help partition write

class SomeManager
{
    inline static CommandEntry commands_[] = {
        { "partition", "list",  &InvokeCommand<&SomeManager::Cmd_List>  },
        { "partition", "write", &InvokeCommand<&SomeManager::Cmd_Write> },
        { "partition", "read",  &InvokeCommand<&SomeManager::Cmd_Read>  },
    };

    // ARG_DONE ends the argument block. Normally it just passes; under `help` the
    // pulls print themselves instead of fetching and this returns early, so the
    // body is never reached. Every handler needs it, including ones that take no
    // arguments — forget it and `help` runs the command.
    RequestError Cmd_List(Args& args, Stream& in, Stream& out)
    {
        ARG_DONE(args);

        // … emit the list …
        return RequestError::Ok;
    }

    RequestError Cmd_Write(Args& args, Stream& in, Stream& out)
    {
        char     partition[17] = {};   // 17 because a label is 17
        uint32_t address       = 0;    // the default, visible where it applies

        ARG_REQUIRED(args.string("partition", partition, sizeof(partition)));
        ARG_OPTIONAL(args.uint32("address",   address));
        ARG_DONE(args);

        // `in` arrives positioned at the body, past the newline.
        // … stream it into the partition, reply with what was written …
        return RequestError::Ok;
    }

    RequestError Cmd_Read(Args& args, Stream& in, Stream& out)
    {
        char     partition[17] = {};
        uint32_t address = 0, length = 0;
        bool     ascii   = false;

        ARG_REQUIRED(args.string("partition", partition, sizeof(partition)));
        ARG_REQUIRED(args.uint32("address",   address));
        ARG_REQUIRED(args.uint32("length",    length));
        ARG_OPTIONAL(args.flag  ("ascii",     ascii));
        ARG_DONE(args);

        // Reading past the end is MEANING, not form — the framework has no idea how
        // big this partition is. So not a RequestError: read what exists, then say
        // in the reply what went wrong and how far we got. The request was fine.
        return RequestError::Ok;
    }

    // `help` is a framework command, nothing to register here. It walks the registry
    // for names, and for one command re-dispatches it with an outputter as `Args`:
    //
    //   > help partition write
    //   partition write
    //     -partition <string>   required
    //     -address   <uint32>   optional
    //
    // Generated from the pulls, so it cannot drift. What a pull cannot express is
    // meaning — that address defaults to 0, that you clear first when resuming — so
    // a category may register prose for that, and only that.
};
