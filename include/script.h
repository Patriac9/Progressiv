//
// Created by zagym on 09/08/2026.
//
#include <utility>
#include <vector>
#include "Binance_interface.h"
#include <Progressiv.h>

#ifndef PROGRESSIV_SCRIPT_H
#define PROGRESSIV_SCRIPT_H
class script
{
public:
    script() = default;
    ~script() = default;
    virtual void init(std::string instId);
    virtual void run();
    virtual void destroy();
    inline void set_asks(std::vector<orderbook_level> ask){asks = std::move(ask);}
    inline void set_bids(std::vector<orderbook_level> bid){bids = std::move(bid);}
    inline void set_controller(Progressiv* controller){progressiv_ = controller;}


protected:
    std::vector<orderbook_level> asks;
    std::vector<orderbook_level> bids;
    std::vector<orderbook_level> trade_asks;
    std::vector<orderbook_level> trade_bids;
    uint64_t current_tick = 0;
    uint64_t last_tick = 0;

protected:
    Progressiv* progressiv_;

};


#endif //PROGRESSIV_SCRIPT_H
