/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <net/route.h>

#include "nat.hh"
#include "util.hh"
#include "interfaces.hh"
#include "address.hh"
#include "dns_proxy.hh"
#include "http_proxy.hh"
#include "netdevice.hh"
#include "event_loop.hh"

using namespace std;

int main( int argc, char *argv[] )
{
    try {
        /* clear environment */
        char **user_environment = environ;
        environ = nullptr;

        check_requirements( argc, argv );

        if ( argc != 2 ) {
            throw Exception( "Usage", string( argv[ 0 ] ) + " folder_for_recorded_content" );
        }

        /* Make sure directory ends with '/' so we can prepend directory to file name for storage */
        string directory( argv[ 1 ] );
        if ( directory.back() != '/' ) {
            directory.append( "/" );
        }

        const Address nameserver = first_nameserver();

        /* set egress and ingress ip addresses */
        Interfaces interfaces;

        auto egress_octet = interfaces.first_unassigned_address( 1 );
        auto ingress_octet = interfaces.first_unassigned_address( egress_octet.second + 1 );

        Address egress_addr = egress_octet.first, ingress_addr = ingress_octet.first;

        /* make pair of devices */
        string egress_name = "veth-" + to_string( getpid() ), ingress_name = "veth-i" + to_string( getpid() );
        VirtualEthernetPair veth_devices( egress_name, ingress_name );

        /* bring up egress */
        assign_address( egress_name, egress_addr, ingress_addr );

        /* create DNS proxy */
        DNSProxy dns_outside( egress_addr, nameserver, nameserver );

        /* set up NAT between egress and eth0 */
        NAT nat_rule( ingress_addr );

        /* set up http proxy for tcp */
        HTTPProxy http_proxy( egress_addr, directory );

        /* set up dnat */
        DNAT dnat( http_proxy.tcp_listener().local_addr(), egress_name );

        /* prepare event loop */
        EventLoop outer_event_loop;

        /* Fork */
        {
            ChildProcess container_process( [&]() {
                    /* bring up localhost */
                    interface_ioctl( Socket( UDP ).fd(), SIOCSIFFLAGS, "lo",
                                     [] ( ifreq &ifr ) { ifr.ifr_flags = IFF_UP; } );

                    /* create DNS proxy if nameserver address is local */
                    auto dns_inside = DNSProxy::maybe_proxy( nameserver,
                                                             dns_outside.udp_listener().local_addr(),
                                                             dns_outside.tcp_listener().local_addr() );

                    /* Fork again after dropping root privileges */
                    drop_privileges();

                    /* prepare child's event loop */
                    EventLoop shell_event_loop;

                    shell_event_loop.add_child_process( [&]() {
                            /* restore environment and tweak prompt */
                            environ = user_environment;
                            prepend_shell_prefix( "[record] " );

                            const string shell = shell_path();
                            SystemCall( "execl", execl( shell.c_str(), shell.c_str(), static_cast<char *>( nullptr ) ) );
                            return EXIT_FAILURE;
                        } );

                    if ( dns_inside ) {
                        dns_inside->register_handlers( shell_event_loop );
                    }

                    return shell_event_loop.loop();
                }, true ); /* new network namespace */

            /* give ingress to container */
            run( { IP, "link", "set", "dev", ingress_name, "netns", to_string( container_process.pid() ) } );
            veth_devices.set_kernel_will_destroy();


            /* bring up ingress */
            in_network_namespace( container_process.pid(), [&] () {
                    /* bring up veth device */
                    assign_address( ingress_name, ingress_addr, egress_addr );

                    /* create default route */
                    rtentry route;
                    zero( route );

                    route.rt_gateway = egress_addr.raw_sockaddr();
                    route.rt_dst = route.rt_genmask = Address().raw_sockaddr();
                    route.rt_flags = RTF_UP | RTF_GATEWAY;

                    SystemCall( "ioctl SIOCADDRT", ioctl( Socket( UDP ).fd().num(), SIOCADDRT, &route ) );
                } );

            /* now that we have its pid, move container process to event loop */
            outer_event_loop.add_child_process( move( container_process ) );
        }

        /* do the actual recording in a different unprivileged child */
        outer_event_loop.add_child_process( [&]() {
                drop_privileges();

                /* check if user-specified storage folder exists, and if not, create it */
                check_storage_folder( directory );

                EventLoop recordr_event_loop;
                dns_outside.register_handlers( recordr_event_loop );
                http_proxy.register_handlers( recordr_event_loop );
                return recordr_event_loop.loop();
            } );

        return outer_event_loop.loop();
    } catch ( const Exception & e ) {
        e.perror();
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
