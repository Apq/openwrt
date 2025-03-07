/*
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all copies.
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/bitops.h>
#include <linux/switch.h>
#include <linux/delay.h>
#include <linux/phy.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/lockdep.h>
#include <linux/workqueue.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_mdio.h>
#include <linux/mdio.h>
#include <linux/gpio.h>

#include "ar40xx.h"

static struct ar40xx_priv *ar40xx_priv;

#define MIB_DESC(_s , _o, _n)	\
	{			\
		.size = (_s),	\
		.offset = (_o),	\
		.name = (_n),	\
	}

static const struct ar40xx_mib_desc ar40xx_mibs[] = {
	MIB_DESC(1, AR40XX_STATS_RXBROAD, "RxBroad"),
	MIB_DESC(1, AR40XX_STATS_RXPAUSE, "RxPause"),
	MIB_DESC(1, AR40XX_STATS_RXMULTI, "RxMulti"),
	MIB_DESC(1, AR40XX_STATS_RXFCSERR, "RxFcsErr"),
	MIB_DESC(1, AR40XX_STATS_RXALIGNERR, "RxAlignErr"),
	MIB_DESC(1, AR40XX_STATS_RXRUNT, "RxRunt"),
	MIB_DESC(1, AR40XX_STATS_RXFRAGMENT, "RxFragment"),
	MIB_DESC(1, AR40XX_STATS_RX64BYTE, "Rx64Byte"),
	MIB_DESC(1, AR40XX_STATS_RX128BYTE, "Rx128Byte"),
	MIB_DESC(1, AR40XX_STATS_RX256BYTE, "Rx256Byte"),
	MIB_DESC(1, AR40XX_STATS_RX512BYTE, "Rx512Byte"),
	MIB_DESC(1, AR40XX_STATS_RX1024BYTE, "Rx1024Byte"),
	MIB_DESC(1, AR40XX_STATS_RX1518BYTE, "Rx1518Byte"),
	MIB_DESC(1, AR40XX_STATS_RXMAXBYTE, "RxMaxByte"),
	MIB_DESC(1, AR40XX_STATS_RXTOOLONG, "RxTooLong"),
	MIB_DESC(2, AR40XX_STATS_RXGOODBYTE, "RxGoodByte"),
	MIB_DESC(2, AR40XX_STATS_RXBADBYTE, "RxBadByte"),
	MIB_DESC(1, AR40XX_STATS_RXOVERFLOW, "RxOverFlow"),
	MIB_DESC(1, AR40XX_STATS_FILTERED, "Filtered"),
	MIB_DESC(1, AR40XX_STATS_TXBROAD, "TxBroad"),
	MIB_DESC(1, AR40XX_STATS_TXPAUSE, "TxPause"),
	MIB_DESC(1, AR40XX_STATS_TXMULTI, "TxMulti"),
	MIB_DESC(1, AR40XX_STATS_TXUNDERRUN, "TxUnderRun"),
	MIB_DESC(1, AR40XX_STATS_TX64BYTE, "Tx64Byte"),
	MIB_DESC(1, AR40XX_STATS_TX128BYTE, "Tx128Byte"),
	MIB_DESC(1, AR40XX_STATS_TX256BYTE, "Tx256Byte"),
	MIB_DESC(1, AR40XX_STATS_TX512BYTE, "Tx512Byte"),
	MIB_DESC(1, AR40XX_STATS_TX1024BYTE, "Tx1024Byte"),
	MIB_DESC(1, AR40XX_STATS_TX1518BYTE, "Tx1518Byte"),
	MIB_DESC(1, AR40XX_STATS_TXMAXBYTE, "TxMaxByte"),
	MIB_DESC(1, AR40XX_STATS_TXOVERSIZE, "TxOverSize"),
	MIB_DESC(2, AR40XX_STATS_TXBYTE, "TxByte"),
	MIB_DESC(1, AR40XX_STATS_TXCOLLISION, "TxCollision"),
	MIB_DESC(1, AR40XX_STATS_TXABORTCOL, "TxAbortCol"),
	MIB_DESC(1, AR40XX_STATS_TXMULTICOL, "TxMultiCol"),
	MIB_DESC(1, AR40XX_STATS_TXSINGLECOL, "TxSingleCol"),
	MIB_DESC(1, AR40XX_STATS_TXEXCDEFER, "TxExcDefer"),
	MIB_DESC(1, AR40XX_STATS_TXDEFER, "TxDefer"),
	MIB_DESC(1, AR40XX_STATS_TXLATECOL, "TxLateCol"),
};

static u32
ar40xx_read(struct ar40xx_priv *priv, int reg)
{
	return readl(priv->hw_addr + reg);
}

static u32
ar40xx_psgmii_read(struct ar40xx_priv *priv, int reg)
{
	return readl(priv->psgmii_hw_addr + reg);
}

static void
ar40xx_write(struct ar40xx_priv *priv, int reg, u32 val)
{
	writel(val, priv->hw_addr + reg);
}

static u32
ar40xx_rmw(struct ar40xx_priv *priv, int reg, u32 mask, u32 val)
{
	u32 ret;

	ret = ar40xx_read(priv, reg);
	ret &= ~mask;
	ret |= val;
	ar40xx_write(priv, reg, ret);
	return ret;
}

static void
ar40xx_psgmii_write(struct ar40xx_priv *priv, int reg, u32 val)
{
	writel(val, priv->psgmii_hw_addr + reg);
}

static void
ar40xx_phy_dbg_write(struct ar40xx_priv *priv, int phy_addr,
		     u16 dbg_addr, u16 dbg_data)
{
	struct mii_bus *bus = priv->mii_bus;

	mutex_lock(&bus->mdio_lock);
	bus->write(bus, phy_addr, AR40XX_MII_ATH_DBG_ADDR, dbg_addr);
	bus->write(bus, phy_addr, AR40XX_MII_ATH_DBG_DATA, dbg_data);
	mutex_unlock(&bus->mdio_lock);
}

static void
ar40xx_phy_dbg_read(struct ar40xx_priv *priv, int phy_addr,
		    u16 dbg_addr, u16 *dbg_data)
{
	struct mii_bus *bus = priv->mii_bus;

	mutex_lock(&bus->mdio_lock);
	bus->write(bus, phy_addr, AR40XX_MII_ATH_DBG_ADDR, dbg_addr);
	*dbg_data = bus->read(bus, phy_addr, AR40XX_MII_ATH_DBG_DATA);
	mutex_unlock(&bus->mdio_lock);
}

static void
ar40xx_phy_mmd_write(struct ar40xx_priv *priv, u32 phy_id,
		     u16 mmd_num, u16 reg_id, u16 reg_val)
{
	struct mii_bus *bus = priv->mii_bus;

	mutex_lock(&bus->mdio_lock);
	bus->write(bus, phy_id,
			AR40XX_MII_ATH_MMD_ADDR, mmd_num);
	bus->write(bus, phy_id,
			AR40XX_MII_ATH_MMD_DATA, reg_id);
	bus->write(bus, phy_id,
			AR40XX_MII_ATH_MMD_ADDR,
			0x4000 | mmd_num);
	bus->write(bus, phy_id,
		AR40XX_MII_ATH_MMD_DATA, reg_val);
	mutex_unlock(&bus->mdio_lock);
}

static u16
ar40xx_phy_mmd_read(struct ar40xx_priv *priv, u32 phy_id,
		    u16 mmd_num, u16 reg_id)
{
	u16 value;
	struct mii_bus *bus = priv->mii_bus;

	mutex_lock(&bus->mdio_lock);
	bus->write(bus, phy_id,
			AR40XX_MII_ATH_MMD_ADDR, mmd_num);
	bus->write(bus, phy_id,
			AR40XX_MII_ATH_MMD_DATA, reg_id);
	bus->write(bus, phy_id,
			AR40XX_MII_ATH_MMD_ADDR,
			0x4000 | mmd_num);
	value = bus->read(bus, phy_id, AR40XX_MII_ATH_MMD_DATA);
	mutex_unlock(&bus->mdio_lock);
	return value;
}

/* Start of swconfig support */

static void
ar40xx_phy_poll_reset(struct ar40xx_priv *priv)
{
	u32 i, in_reset, retries = 500;
	struct mii_bus *bus = priv->mii_bus;

	/* Assume RESET was recently issued to some or all of the phys */
	in_reset = GENMASK(AR40XX_NUM_PHYS - 1, 0);

	while (retries--) {
		/* 1ms should be plenty of time.
		 * 802.3 spec allows for a max wait time of 500ms
		 */
		usleep_range(1000, 2000);

		for (i = 0; i < AR40XX_NUM_PHYS; i++) {
			int val;

			/* skip devices which have completed reset */
			if (!(in_reset & BIT(i)))
				continue;

			val = mdiobus_read(bus, i, MII_BMCR);
			if (val < 0)
				continue;

			/* mark when phy is no longer in reset state */
			if (!(val & BMCR_RESET))
				in_reset &= ~BIT(i);
		}

		if (!in_reset)
			return;
	}

	dev_warn(&bus->dev, "Failed to reset all phys! (in_reset: 0x%x)\n",
		 in_reset);
}

static void
ar40xx_phy_init(struct ar40xx_priv *priv)
{
	int i;
	struct mii_bus *bus;
	u16 val;

	bus = priv->mii_bus;
	for (i = 0; i < AR40XX_NUM_PORTS - 1; i++) {
		ar40xx_phy_dbg_read(priv, i, AR40XX_PHY_DEBUG_0, &val);
		val &= ~AR40XX_PHY_MANU_CTRL_EN;
		ar40xx_phy_dbg_write(priv, i, AR40XX_PHY_DEBUG_0, val);
		mdiobus_write(bus, i,
			      MII_ADVERTISE, ADVERTISE_ALL |
			      ADVERTISE_PAUSE_CAP |
			      ADVERTISE_PAUSE_ASYM);
		mdiobus_write(bus, i, MII_CTRL1000, ADVERTISE_1000FULL);
		mdiobus_write(bus, i, MII_BMCR, BMCR_RESET | BMCR_ANENABLE);
	}

	ar40xx_phy_poll_reset(priv);
}

static void
ar40xx_port_phy_linkdown(struct ar40xx_priv *priv)
{
	struct mii_bus *bus;
	int i;
	u16 val;

	bus = priv->mii_bus;
	for (i = 0; i < AR40XX_NUM_PORTS - 1; i++) {
		mdiobus_write(bus, i, MII_CTRL1000, 0);
		mdiobus_write(bus, i, MII_ADVERTISE, 0);
		mdiobus_write(bus, i, MII_BMCR, BMCR_RESET | BMCR_ANENABLE);
		ar40xx_phy_dbg_read(priv, i, AR40XX_PHY_DEBUG_0, &val);
		val |= AR40XX_PHY_MANU_CTRL_EN;
		ar40xx_phy_dbg_write(priv, i, AR40XX_PHY_DEBUG_0, val);
		/* disable transmit */
		ar40xx_phy_dbg_read(priv, i, AR40XX_PHY_DEBUG_2, &val);
		val &= 0xf00f;
		ar40xx_phy_dbg_write(priv, i, AR40XX_PHY_DEBUG_2, val);
	}
}

static void
ar40xx_set_mirror_regs(struct ar40xx_priv *priv)
{
	int port;

	/* reset all mirror registers */
	ar40xx_rmw(priv, AR40XX_REG_FWD_CTRL0,
		   AR40XX_FWD_CTRL0_MIRROR_PORT,
		   (0xF << AR40XX_FWD_CTRL0_MIRROR_PORT_S));
	for (port = 0; port < AR40XX_NUM_PORTS; port++) {
		ar40xx_rmw(priv, AR40XX_REG_PORT_LOOKUP(port),
			   AR40XX_PORT_LOOKUP_ING_MIRROR_EN, 0);

		ar40xx_rmw(priv, AR40XX_REG_PORT_HOL_CTRL1(port),
			   AR40XX_PORT_HOL_CTRL1_EG_MIRROR_EN, 0);
	}

	/* now enable mirroring if necessary */
	if (priv->source_port >= AR40XX_NUM_PORTS ||
	    priv->monitor_port >= AR40XX_NUM_PORTS ||
	    priv->source_port == priv->monitor_port) {
		return;
	}

	ar40xx_rmw(priv, AR40XX_REG_FWD_CTRL0,
		   AR40XX_FWD_CTRL0_MIRROR_PORT,
		   (priv->monitor_port << AR40XX_FWD_CTRL0_MIRROR_PORT_S));

	if (priv->mirror_rx)
		ar40xx_rmw(priv, AR40XX_REG_PORT_LOOKUP(priv->source_port), 0,
			   AR40XX_PORT_LOOKUP_ING_MIRROR_EN);

	if (priv->mirror_tx)
		ar40xx_rmw(priv, AR40XX_REG_PORT_HOL_CTRL1(priv->source_port),
			   0, AR40XX_PORT_HOL_CTRL1_EG_MIRROR_EN);
}

static int
ar40xx_sw_get_ports(struct switch_dev *dev, struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);
	u8 ports = priv->vlan_table[val->port_vlan];
	int i;

	val->len = 0;
	for (i = 0; i < dev->ports; i++) {
		struct switch_port *p;

		if (!(ports & BIT(i)))
			continue;

		p = &val->value.ports[val->len++];
		p->id = i;
		if ((priv->vlan_tagged & BIT(i)) ||
		    (priv->pvid[i] != val->port_vlan))
			p->flags = BIT(SWITCH_PORT_FLAG_TAGGED);
		else
			p->flags = 0;
	}
	return 0;
}

static int
ar40xx_sw_set_ports(struct switch_dev *dev, struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);
	u8 *vt = &priv->vlan_table[val->port_vlan];
	int i;

	*vt = 0;
	for (i = 0; i < val->len; i++) {
		struct switch_port *p = &val->value.ports[i];

		if (p->flags & BIT(SWITCH_PORT_FLAG_TAGGED)) {
			if (val->port_vlan == priv->pvid[p->id])
				priv->vlan_tagged |= BIT(p->id);
		} else {
			priv->vlan_tagged &= ~BIT(p->id);
			priv->pvid[p->id] = val->port_vlan;
		}

		*vt |= BIT(p->id);
	}
	return 0;
}

static int
ar40xx_reg_wait(struct ar40xx_priv *priv, u32 reg, u32 mask, u32 val,
		unsigned timeout)
{
	int i;

	for (i = 0; i < timeout; i++) {
		u32 t;

		t = ar40xx_read(priv, reg);
		if ((t & mask) == val)
			return 0;

		usleep_range(1000, 2000);
	}

	return -ETIMEDOUT;
}

static int
ar40xx_mib_op(struct ar40xx_priv *priv, u32 op)
{
	int ret;

	lockdep_assert_held(&priv->mib_lock);

	/* Capture the hardware statistics for all ports */
	ar40xx_rmw(priv, AR40XX_REG_MIB_FUNC,
		   AR40XX_MIB_FUNC, (op << AR40XX_MIB_FUNC_S));

	/* Wait for the capturing to complete. */
	ret = ar40xx_reg_wait(priv, AR40XX_REG_MIB_FUNC,
			      AR40XX_MIB_BUSY, 0, 10);

	return ret;
}

static void
ar40xx_mib_fetch_port_stat(struct ar40xx_priv *priv, int port, bool flush)
{
	unsigned int base;
	u64 *mib_stats;
	int i;
	u32 num_mibs = ARRAY_SIZE(ar40xx_mibs);

	WARN_ON(port >= priv->dev.ports);

	lockdep_assert_held(&priv->mib_lock);

	base = AR40XX_REG_PORT_STATS_START +
	       AR40XX_REG_PORT_STATS_LEN * port;

	mib_stats = &priv->mib_stats[port * num_mibs];
	if (flush) {
		u32 len;

		len = num_mibs * sizeof(*mib_stats);
		memset(mib_stats, 0, len);
		return;
	}
	for (i = 0; i < num_mibs; i++) {
		const struct ar40xx_mib_desc *mib;
		u64 t;

		mib = &ar40xx_mibs[i];
		t = ar40xx_read(priv, base + mib->offset);
		if (mib->size == 2) {
			u64 hi;

			hi = ar40xx_read(priv, base + mib->offset + 4);
			t |= hi << 32;
		}

		mib_stats[i] += t;
	}
}

static int
ar40xx_mib_capture(struct ar40xx_priv *priv)
{
	return ar40xx_mib_op(priv, AR40XX_MIB_FUNC_CAPTURE);
}

static int
ar40xx_mib_flush(struct ar40xx_priv *priv)
{
	return ar40xx_mib_op(priv, AR40XX_MIB_FUNC_FLUSH);
}

static int
ar40xx_sw_set_reset_mibs(struct switch_dev *dev,
			 const struct switch_attr *attr,
			 struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);
	unsigned int len;
	int ret;
	u32 num_mibs = ARRAY_SIZE(ar40xx_mibs);

	mutex_lock(&priv->mib_lock);

	len = priv->dev.ports * num_mibs * sizeof(*priv->mib_stats);
	memset(priv->mib_stats, 0, len);
	ret = ar40xx_mib_flush(priv);

	mutex_unlock(&priv->mib_lock);
	return ret;
}

static int
ar40xx_sw_set_vlan(struct switch_dev *dev, const struct switch_attr *attr,
		   struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);

	priv->vlan = !!val->value.i;
	return 0;
}

static int
ar40xx_sw_get_vlan(struct switch_dev *dev, const struct switch_attr *attr,
		   struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);

	val->value.i = priv->vlan;
	return 0;
}

static int
ar40xx_sw_set_mirror_rx_enable(struct switch_dev *dev,
			       const struct switch_attr *attr,
			       struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);

	mutex_lock(&priv->reg_mutex);
	priv->mirror_rx = !!val->value.i;
	ar40xx_set_mirror_regs(priv);
	mutex_unlock(&priv->reg_mutex);

	return 0;
}

static int
ar40xx_sw_get_mirror_rx_enable(struct switch_dev *dev,
			       const struct switch_attr *attr,
			       struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);

	mutex_lock(&priv->reg_mutex);
	val->value.i = priv->mirror_rx;
	mutex_unlock(&priv->reg_mutex);
	return 0;
}

static int
ar40xx_sw_set_mirror_tx_enable(struct switch_dev *dev,
			       const struct switch_attr *attr,
			       struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);

	mutex_lock(&priv->reg_mutex);
	priv->mirror_tx = !!val->value.i;
	ar40xx_set_mirror_regs(priv);
	mutex_unlock(&priv->reg_mutex);

	return 0;
}

static int
ar40xx_sw_get_mirror_tx_enable(struct switch_dev *dev,
			       const struct switch_attr *attr,
			       struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);

	mutex_lock(&priv->reg_mutex);
	val->value.i = priv->mirror_tx;
	mutex_unlock(&priv->reg_mutex);
	return 0;
}

static int
ar40xx_sw_set_mirror_monitor_port(struct switch_dev *dev,
				  const struct switch_attr *attr,
				  struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);

	mutex_lock(&priv->reg_mutex);
	priv->monitor_port = val->value.i;
	ar40xx_set_mirror_regs(priv);
	mutex_unlock(&priv->reg_mutex);

	return 0;
}

static int
ar40xx_sw_get_mirror_monitor_port(struct switch_dev *dev,
				  const struct switch_attr *attr,
				  struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);

	mutex_lock(&priv->reg_mutex);
	val->value.i = priv->monitor_port;
	mutex_unlock(&priv->reg_mutex);
	return 0;
}

static int
ar40xx_sw_set_mirror_source_port(struct switch_dev *dev,
				 const struct switch_attr *attr,
				 struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);

	mutex_lock(&priv->reg_mutex);
	priv->source_port = val->value.i;
	ar40xx_set_mirror_regs(priv);
	mutex_unlock(&priv->reg_mutex);

	return 0;
}

static int
ar40xx_sw_get_mirror_source_port(struct switch_dev *dev,
				 const struct switch_attr *attr,
				 struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);

	mutex_lock(&priv->reg_mutex);
	val->value.i = priv->source_port;
	mutex_unlock(&priv->reg_mutex);
	return 0;
}

static int
ar40xx_sw_set_linkdown(struct switch_dev *dev,
		       const struct switch_attr *attr,
		       struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);

	if (val->value.i == 1)
		ar40xx_port_phy_linkdown(priv);
	else
		ar40xx_phy_init(priv);

	return 0;
}

static int
ar40xx_sw_set_port_reset_mib(struct switch_dev *dev,
			     const struct switch_attr *attr,
			     struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);
	int port;
	int ret;

	port = val->port_vlan;
	if (port >= dev->ports)
		return -EINVAL;

	mutex_lock(&priv->mib_lock);
	ret = ar40xx_mib_capture(priv);
	if (ret)
		goto unlock;

	ar40xx_mib_fetch_port_stat(priv, port, true);

unlock:
	mutex_unlock(&priv->mib_lock);
	return ret;
}

static int
ar40xx_sw_get_port_mib(struct switch_dev *dev,
		       const struct switch_attr *attr,
		       struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);
	u64 *mib_stats;
	int port;
	int ret;
	char *buf = priv->buf;
	int i, len = 0;
	u32 num_mibs = ARRAY_SIZE(ar40xx_mibs);

	port = val->port_vlan;
	if (port >= dev->ports)
		return -EINVAL;

	mutex_lock(&priv->mib_lock);
	ret = ar40xx_mib_capture(priv);
	if (ret)
		goto unlock;

	ar40xx_mib_fetch_port_stat(priv, port, false);

	len += snprintf(buf + len, sizeof(priv->buf) - len,
			"Port %d MIB counters\n",
			port);

	mib_stats = &priv->mib_stats[port * num_mibs];
	for (i = 0; i < num_mibs; i++)
		len += snprintf(buf + len, sizeof(priv->buf) - len,
				"%-12s: %llu\n",
				ar40xx_mibs[i].name,
				mib_stats[i]);

	val->value.s = buf;
	val->len = len;

unlock:
	mutex_unlock(&priv->mib_lock);
	return ret;
}

static int
ar40xx_sw_set_vid(struct switch_dev *dev, const struct switch_attr *attr,
		  struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);

	priv->vlan_id[val->port_vlan] = val->value.i;
	return 0;
}

static int
ar40xx_sw_get_vid(struct switch_dev *dev, const struct switch_attr *attr,
		  struct switch_val *val)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);

	val->value.i = priv->vlan_id[val->port_vlan];
	return 0;
}

static int
ar40xx_sw_get_pvid(struct switch_dev *dev, int port, int *vlan)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);
	*vlan = priv->pvid[port];
	return 0;
}

static int
ar40xx_sw_set_pvid(struct switch_dev *dev, int port, int vlan)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);

	/* make sure no invalid PVIDs get set */
	if (vlan >= dev->vlans)
		return -EINVAL;

	priv->pvid[port] = vlan;
	return 0;
}

static void
ar40xx_read_port_link(struct ar40xx_priv *priv, int port,
		      struct switch_port_link *link)
{
	u32 status;
	u32 speed;

	memset(link, 0, sizeof(*link));

	status = ar40xx_read(priv, AR40XX_REG_PORT_STATUS(port));

	link->aneg = !!(status & AR40XX_PORT_AUTO_LINK_EN);
	if (link->aneg || (port != AR40XX_PORT_CPU))
		link->link = !!(status & AR40XX_PORT_STATUS_LINK_UP);
	else
		link->link = true;

	if (!link->link)
		return;

	link->duplex = !!(status & AR40XX_PORT_DUPLEX);
	link->tx_flow = !!(status & AR40XX_PORT_STATUS_TXFLOW);
	link->rx_flow = !!(status & AR40XX_PORT_STATUS_RXFLOW);

	speed = (status & AR40XX_PORT_SPEED) >>
		 AR40XX_PORT_STATUS_SPEED_S;

	switch (speed) {
	case AR40XX_PORT_SPEED_10M:
		link->speed = SWITCH_PORT_SPEED_10;
		break;
	case AR40XX_PORT_SPEED_100M:
		link->speed = SWITCH_PORT_SPEED_100;
		break;
	case AR40XX_PORT_SPEED_1000M:
		link->speed = SWITCH_PORT_SPEED_1000;
		break;
	default:
		link->speed = SWITCH_PORT_SPEED_UNKNOWN;
		break;
	}
}

static int
ar40xx_sw_get_port_link(struct switch_dev *dev, int port,
			struct switch_port_link *link)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);

	ar40xx_read_port_link(priv, port, link);
	return 0;
}

static int
ar40xx_sw_get_port_stats(struct switch_dev *dev, int port,
			struct switch_port_stats *stats)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);
	u32 num_mibs = ARRAY_SIZE(ar40xx_mibs);
	u64 *mib_stats;
	int ret = 0;
	static int last_port = AR40XX_NUM_PORTS - 1;

	mutex_lock(&priv->mib_lock);

	/* Avoid unnecessary capture */
	if (last_port >= port) {
		ret = ar40xx_mib_capture(priv);
		if (ret)
			goto unlock;
	}
	last_port = port;

	ar40xx_mib_fetch_port_stat(priv, port, false);

	mib_stats = &priv->mib_stats[port * num_mibs];
	/* 15: TXBYTE, 31: RXGOODBYTE */
	stats->tx_bytes = mib_stats[15];
	stats->rx_bytes = mib_stats[31];
unlock:
	mutex_unlock(&priv->mib_lock);
	return ret;
}

static const struct switch_attr ar40xx_sw_attr_globals[] = {
	{
		.type = SWITCH_TYPE_INT,
		.name = "enable_vlan",
		.description = "Enable VLAN mode",
		.set = ar40xx_sw_set_vlan,
		.get = ar40xx_sw_get_vlan,
		.max = 1
	},
	{
		.type = SWITCH_TYPE_NOVAL,
		.name = "reset_mibs",
		.description = "Reset all MIB counters",
		.set = ar40xx_sw_set_reset_mibs,
	},
	{
		.type = SWITCH_TYPE_INT,
		.name = "enable_mirror_rx",
		.description = "Enable mirroring of RX packets",
		.set = ar40xx_sw_set_mirror_rx_enable,
		.get = ar40xx_sw_get_mirror_rx_enable,
		.max = 1
	},
	{
		.type = SWITCH_TYPE_INT,
		.name = "enable_mirror_tx",
		.description = "Enable mirroring of TX packets",
		.set = ar40xx_sw_set_mirror_tx_enable,
		.get = ar40xx_sw_get_mirror_tx_enable,
		.max = 1
	},
	{
		.type = SWITCH_TYPE_INT,
		.name = "mirror_monitor_port",
		.description = "Mirror monitor port",
		.set = ar40xx_sw_set_mirror_monitor_port,
		.get = ar40xx_sw_get_mirror_monitor_port,
		.max = AR40XX_NUM_PORTS - 1
	},
	{
		.type = SWITCH_TYPE_INT,
		.name = "mirror_source_port",
		.description = "Mirror source port",
		.set = ar40xx_sw_set_mirror_source_port,
		.get = ar40xx_sw_get_mirror_source_port,
		.max = AR40XX_NUM_PORTS - 1
	},
	{
		.type = SWITCH_TYPE_INT,
		.name = "linkdown",
		.description = "Link down all the PHYs",
		.set = ar40xx_sw_set_linkdown,
		.max = 1
	},
};

static const struct switch_attr ar40xx_sw_attr_port[] = {
	{
		.type = SWITCH_TYPE_NOVAL,
		.name = "reset_mib",
		.description = "Reset single port MIB counters",
		.set = ar40xx_sw_set_port_reset_mib,
	},
	{
		.type = SWITCH_TYPE_STRING,
		.name = "mib",
		.description = "Get port's MIB counters",
		.set = NULL,
		.get = ar40xx_sw_get_port_mib,
	},
};

const struct switch_attr ar40xx_sw_attr_vlan[] = {
	{
		.type = SWITCH_TYPE_INT,
		.name = "vid",
		.description = "VLAN ID (0-4094)",
		.set = ar40xx_sw_set_vid,
		.get = ar40xx_sw_get_vid,
		.max = 4094,
	},
};

/* End of swconfig support */

static int
ar40xx_wait_bit(struct ar40xx_priv *priv, int reg, u32 mask, u32 val)
{
	int timeout = 20;
	u32 t;

	while (1) {
		t = ar40xx_read(priv, reg);
		if ((t & mask) == val)
			return 0;

		if (timeout-- <= 0)
			break;

		usleep_range(10, 20);
	}

	pr_err("ar40xx: timeout for reg %08x: %08x & %08x != %08x\n",
	       (unsigned int)reg, t, mask, val);
	return -ETIMEDOUT;
}

static int
ar40xx_atu_flush(struct ar40xx_priv *priv)
{
	int ret;

	ret = ar40xx_wait_bit(priv, AR40XX_REG_ATU_FUNC,
			      AR40XX_ATU_FUNC_BUSY, 0);
	if (!ret)
		ar40xx_write(priv, AR40XX_REG_ATU_FUNC,
			     AR40XX_ATU_FUNC_OP_FLUSH |
			     AR40XX_ATU_FUNC_BUSY);

	return ret;
}

static void
ar40xx_ess_reset(struct ar40xx_priv *priv)
{
	reset_control_assert(priv->ess_rst);
	mdelay(10);
	reset_control_deassert(priv->ess_rst);
	/* Waiting for all inner tables init done.
	  * It cost 5~10ms.
	  */
	mdelay(10);

	pr_info("ESS reset ok!\n");
}

/* Start of psgmii self test */

static void
ar40xx_malibu_psgmii_ess_reset(struct ar40xx_priv *priv)
{
	u32 n;
	struct mii_bus *bus = priv->mii_bus;
	/* reset phy psgmii */
	/* fix phy psgmii RX 20bit */
	mdiobus_write(bus, 5, 0x0, 0x005b);
	/* reset phy psgmii */
	mdiobus_write(bus, 5, 0x0, 0x001b);
	/* release reset phy psgmii */
	mdiobus_write(bus, 5, 0x0, 0x005b);

	for (n = 0; n < AR40XX_PSGMII_CALB_NUM; n++) {
		u16 status;

		status = ar40xx_phy_mmd_read(priv, 5, 1, 0x28);
		if (status & BIT(0))
			break;
		/* Polling interval to check PSGMII PLL in malibu is ready
		  * the worst time is 8.67ms
		  * for 25MHz reference clock
		  * [512+(128+2048)*49]*80ns+100us
		  */
		mdelay(2);
	}

	/*check malibu psgmii calibration done end..*/

	/*freeze phy psgmii RX CDR*/
	mdiobus_write(bus, 5, 0x1a, 0x2230);

	ar40xx_ess_reset(priv);

	/*check psgmii calibration done start*/
	for (n = 0; n < AR40XX_PSGMII_CALB_NUM; n++) {
		u32 status;

		status = ar40xx_psgmii_read(priv, 0xa0);
		if (status & BIT(0))
			break;
		/* Polling interval to check PSGMII PLL in ESS is ready */
		mdelay(2);
	}

	/* check dakota psgmii calibration done end..*/

	/* relesae phy psgmii RX CDR */
	mdiobus_write(bus, 5, 0x1a, 0x3230);
	/* release phy psgmii RX 20bit */
	mdiobus_write(bus, 5, 0x0, 0x005f);
}

static void
ar40xx_psgmii_single_phy_testing(struct ar40xx_priv *priv, int phy)
{
	int j;
	u32 tx_ok, tx_error;
	u32 rx_ok, rx_error;
	u32 tx_ok_high16;
	u32 rx_ok_high16;
	u32 tx_all_ok, rx_all_ok;
	struct mii_bus *bus = priv->mii_bus;

	mdiobus_write(bus, phy, 0x0, 0x9000);
	mdiobus_write(bus, phy, 0x0, 0x4140);

	for (j = 0; j < AR40XX_PSGMII_CALB_NUM; j++) {
		u16 status;

		status = mdiobus_read(bus, phy, 0x11);
		if (status & AR40XX_PHY_SPEC_STATUS_LINK)
			break;
		/* the polling interval to check if the PHY link up or not
		  * maxwait_timer: 750 ms +/-10 ms
		  * minwait_timer : 1 us +/- 0.1us
		  * time resides in minwait_timer ~ maxwait_timer
		  * see IEEE 802.3 section 40.4.5.2
		  */
		mdelay(8);
	}

	/* enable check */
	ar40xx_phy_mmd_write(priv, phy, 7, 0x8029, 0x0000);
	ar40xx_phy_mmd_write(priv, phy, 7, 0x8029, 0x0003);

	/* start traffic */
	ar40xx_phy_mmd_write(priv, phy, 7, 0x8020, 0xa000);
	/* wait for all traffic end
	  * 4096(pkt num)*1524(size)*8ns(125MHz)=49.9ms
	  */
	mdelay(50);

	/* check counter */
	tx_ok = ar40xx_phy_mmd_read(priv, phy, 7, 0x802e);
	tx_ok_high16 = ar40xx_phy_mmd_read(priv, phy, 7, 0x802d);
	tx_error = ar40xx_phy_mmd_read(priv, phy, 7, 0x802f);
	rx_ok = ar40xx_phy_mmd_read(priv, phy, 7, 0x802b);
	rx_ok_high16 = ar40xx_phy_mmd_read(priv, phy, 7, 0x802a);
	rx_error = ar40xx_phy_mmd_read(priv, phy, 7, 0x802c);
	tx_all_ok = tx_ok + (tx_ok_high16 << 16);
	rx_all_ok = rx_ok + (rx_ok_high16 << 16);
	if (tx_all_ok == 0x1000 && tx_error == 0) {
		/* success */
		priv->phy_t_status &= (~BIT(phy));
	} else {
		pr_info("PHY %d single test PSGMII issue happen!\n", phy);
		priv->phy_t_status |= BIT(phy);
	}

	mdiobus_write(bus, phy, 0x0, 0x1840);
}

static void
ar40xx_psgmii_all_phy_testing(struct ar40xx_priv *priv)
{
	int phy, j;
	struct mii_bus *bus = priv->mii_bus;

	mdiobus_write(bus, 0x1f, 0x0, 0x9000);
	mdiobus_write(bus, 0x1f, 0x0, 0x4140);

	for (j = 0; j < AR40XX_PSGMII_CALB_NUM; j++) {
		for (phy = 0; phy < AR40XX_NUM_PORTS - 1; phy++) {
			u16 status;

			status = mdiobus_read(bus, phy, 0x11);
			if (!(status & BIT(10)))
				break;
		}

		if (phy >= (AR40XX_NUM_PORTS - 1))
			break;
		/* The polling interva to check if the PHY link up or not */
		mdelay(8);
	}
	/* enable check */
	ar40xx_phy_mmd_write(priv, 0x1f, 7, 0x8029, 0x0000);
	ar40xx_phy_mmd_write(priv, 0x1f, 7, 0x8029, 0x0003);

	/* start traffic */
	ar40xx_phy_mmd_write(priv, 0x1f, 7, 0x8020, 0xa000);
	/* wait for all traffic end
	  * 4096(pkt num)*1524(size)*8ns(125MHz)=49.9ms
	  */
	mdelay(50);

	for (phy = 0; phy < AR40XX_NUM_PORTS - 1; phy++) {
		u32 tx_ok, tx_error;
		u32 rx_ok, rx_error;
		u32 tx_ok_high16;
		u32 rx_ok_high16;
		u32 tx_all_ok, rx_all_ok;

		/* check counter */
		tx_ok = ar40xx_phy_mmd_read(priv, phy, 7, 0x802e);
		tx_ok_high16 = ar40xx_phy_mmd_read(priv, phy, 7, 0x802d);
		tx_error = ar40xx_phy_mmd_read(priv, phy, 7, 0x802f);
		rx_ok = ar40xx_phy_mmd_read(priv, phy, 7, 0x802b);
		rx_ok_high16 = ar40xx_phy_mmd_read(priv, phy, 7, 0x802a);
		rx_error = ar40xx_phy_mmd_read(priv, phy, 7, 0x802c);
		tx_all_ok = tx_ok + (tx_ok_high16<<16);
		rx_all_ok = rx_ok + (rx_ok_high16<<16);
		if (tx_all_ok == 0x1000 && tx_error == 0) {
			/* success */
			priv->phy_t_status &= ~BIT(phy + 8);
		} else {
			pr_info("PHY%d test see issue!\n", phy);
			priv->phy_t_status |= BIT(phy + 8);
		}
	}

	pr_debug("PHY all test 0x%x \r\n", priv->phy_t_status);
}

void
ar40xx_psgmii_self_test(struct ar40xx_priv *priv)
{
	u32 i, phy;
	struct mii_bus *bus = priv->mii_bus;

	ar40xx_malibu_psgmii_ess_reset(priv);

	/* switch to access MII reg for copper */
	mdiobus_write(bus, 4, 0x1f, 0x8500);
	for (phy = 0; phy < AR40XX_NUM_PORTS - 1; phy++) {
		/*enable phy mdio broadcast write*/
		ar40xx_phy_mmd_write(priv, phy, 7, 0x8028, 0x801f);
	}
	/* force no link by power down */
	mdiobus_write(bus, 0x1f, 0x0, 0x1840);
	/*packet number*/
	ar40xx_phy_mmd_write(priv, 0x1f, 7, 0x8021, 0x1000);
	ar40xx_phy_mmd_write(priv, 0x1f, 7, 0x8062, 0x05e0);

	/*fix mdi status */
	mdiobus_write(bus, 0x1f, 0x10, 0x6800);
	for (i = 0; i < AR40XX_PSGMII_CALB_NUM; i++) {
		priv->phy_t_status = 0;

		for (phy = 0; phy < AR40XX_NUM_PORTS - 1; phy++) {
			ar40xx_rmw(priv, AR40XX_REG_PORT_LOOKUP(phy + 1),
				AR40XX_PORT_LOOKUP_LOOPBACK,
				AR40XX_PORT_LOOKUP_LOOPBACK);
		}

		for (phy = 0; phy < AR40XX_NUM_PORTS - 1; phy++)
			ar40xx_psgmii_single_phy_testing(priv, phy);

		ar40xx_psgmii_all_phy_testing(priv);

		if (priv->phy_t_status)
			ar40xx_malibu_psgmii_ess_reset(priv);
		else
			break;
	}

	if (i >= AR40XX_PSGMII_CALB_NUM)
		pr_info("PSGMII cannot recover\n");
	else
		pr_debug("PSGMII recovered after %d times reset\n", i);

	/* configuration recover */
	/* packet number */
	ar40xx_phy_mmd_write(priv, 0x1f, 7, 0x8021, 0x0);
	/* disable check */
	ar40xx_phy_mmd_write(priv, 0x1f, 7, 0x8029, 0x0);
	/* disable traffic */
	ar40xx_phy_mmd_write(priv, 0x1f, 7, 0x8020, 0x0);
}

void
ar40xx_psgmii_self_test_clean(struct ar40xx_priv *priv)
{
	int phy;
	struct mii_bus *bus = priv->mii_bus;

	/* disable phy internal loopback */
	mdiobus_write(bus, 0x1f, 0x10, 0x6860);
	mdiobus_write(bus, 0x1f, 0x0, 0x9040);
	ar40xx_phy_mmd_write(priv, 0x1f, 7, 0x8076, 0x0670); /* 1000_LED_n */

	for (phy = 0; phy < AR40XX_NUM_PORTS - 1; phy++) {
		/* disable mac loop back */
		ar40xx_rmw(priv, AR40XX_REG_PORT_LOOKUP(phy + 1),
				AR40XX_PORT_LOOKUP_LOOPBACK, 0);
		/* disable phy mdio broadcast write */
		ar40xx_phy_mmd_write(priv, phy, 7, 0x8028, 0x001f);
	}

	/* clear fdb entry */
	ar40xx_atu_flush(priv);
}

/* End of psgmii self test */

static void
ar40xx_mac_mode_init(struct ar40xx_priv *priv, u32 mode)
{
	if (mode == PORT_WRAPPER_PSGMII) {
		ar40xx_psgmii_write(priv, AR40XX_PSGMII_MODE_CONTROL, 0x2200);
		ar40xx_psgmii_write(priv, AR40XX_PSGMIIPHY_TX_CONTROL, 0x8380);
	}
}

static
int ar40xx_cpuport_setup(struct ar40xx_priv *priv)
{
	u32 t;

	t = AR40XX_PORT_STATUS_TXFLOW |
	     AR40XX_PORT_STATUS_RXFLOW |
	     AR40XX_PORT_TXHALF_FLOW |
	     AR40XX_PORT_DUPLEX |
	     AR40XX_PORT_SPEED_1000M;
	ar40xx_write(priv, AR40XX_REG_PORT_STATUS(0), t);
	usleep_range(10, 20);

	t |= AR40XX_PORT_TX_EN |
	       AR40XX_PORT_RX_EN;
	ar40xx_write(priv, AR40XX_REG_PORT_STATUS(0), t);

	return 0;
}

static void
ar40xx_init_port(struct ar40xx_priv *priv, int port)
{
	u32 t;

	ar40xx_write(priv, AR40XX_REG_PORT_STATUS(port), 0);

	ar40xx_write(priv, AR40XX_REG_PORT_HEADER(port), 0);

	ar40xx_write(priv, AR40XX_REG_PORT_VLAN0(port), 0);

	t = AR40XX_PORT_VLAN1_OUT_MODE_UNTOUCH << AR40XX_PORT_VLAN1_OUT_MODE_S;
	ar40xx_write(priv, AR40XX_REG_PORT_VLAN1(port), t);

	t = AR40XX_PORT_LOOKUP_LEARN;
	t |= AR40XX_PORT_STATE_FORWARD << AR40XX_PORT_LOOKUP_STATE_S;
	ar40xx_write(priv, AR40XX_REG_PORT_LOOKUP(port), t);
}

void
ar40xx_init_globals(struct ar40xx_priv *priv)
{
	u32 t;

	/* enable CPU port and disable mirror port */
	t = AR40XX_FWD_CTRL0_CPU_PORT_EN |
	    AR40XX_FWD_CTRL0_MIRROR_PORT;
	ar40xx_write(priv, AR40XX_REG_FWD_CTRL0, t);

	/* forward multicast and broadcast frames to CPU */
	t = (AR40XX_PORTS_ALL << AR40XX_FWD_CTRL1_UC_FLOOD_S) |
	    (AR40XX_PORTS_ALL << AR40XX_FWD_CTRL1_MC_FLOOD_S) |
	    (AR40XX_PORTS_ALL << AR40XX_FWD_CTRL1_BC_FLOOD_S);
	ar40xx_write(priv, AR40XX_REG_FWD_CTRL1, t);

	/* enable jumbo frames */
	ar40xx_rmw(priv, AR40XX_REG_MAX_FRAME_SIZE,
		   AR40XX_MAX_FRAME_SIZE_MTU, 9018 + 8 + 2);

	/* Enable MIB counters */
	ar40xx_rmw(priv, AR40XX_REG_MODULE_EN, 0,
		   AR40XX_MODULE_EN_MIB);

	/* Disable AZ */
	ar40xx_write(priv, AR40XX_REG_EEE_CTRL, 0);

	/* set flowctrl thershold for cpu port */
	t = (AR40XX_PORT0_FC_THRESH_ON_DFLT << 16) |
	      AR40XX_PORT0_FC_THRESH_OFF_DFLT;
	ar40xx_write(priv, AR40XX_REG_PORT_FLOWCTRL_THRESH(0), t);
}

static int
ar40xx_hw_init(struct ar40xx_priv *priv)
{
	u32 i;

	ar40xx_ess_reset(priv);

	if (!priv->mii_bus)
		return -1;

	ar40xx_psgmii_self_test(priv);
	ar40xx_psgmii_self_test_clean(priv);

	ar40xx_mac_mode_init(priv, priv->mac_mode);

	for (i = 0; i < priv->dev.ports; i++)
		ar40xx_init_port(priv, i);

	ar40xx_init_globals(priv);

	return 0;
}

/* Start of qm error WAR */

static
int ar40xx_force_1g_full(struct ar40xx_priv *priv, u32 port_id)
{
	u32 reg;

	if (port_id < 0 || port_id > 6)
		return -1;

	reg = AR40XX_REG_PORT_STATUS(port_id);
	return ar40xx_rmw(priv, reg, AR40XX_PORT_SPEED,
			(AR40XX_PORT_SPEED_1000M | AR40XX_PORT_DUPLEX));
}

static
int ar40xx_get_qm_status(struct ar40xx_priv *priv,
			 u32 port_id, u32 *qm_buffer_err)
{
	u32 reg;
	u32 qm_val;

	if (port_id < 1 || port_id > 5) {
		*qm_buffer_err = 0;
		return -1;
	}

	if (port_id < 4) {
		reg = AR40XX_REG_QM_PORT0_3_QNUM;
		ar40xx_write(priv, AR40XX_REG_QM_DEBUG_ADDR, reg);
		qm_val = ar40xx_read(priv, AR40XX_REG_QM_DEBUG_VALUE);
		/* every 8 bits for each port */
		*qm_buffer_err = (qm_val >> (port_id * 8)) & 0xFF;
	} else {
		reg = AR40XX_REG_QM_PORT4_6_QNUM;
		ar40xx_write(priv, AR40XX_REG_QM_DEBUG_ADDR, reg);
		qm_val = ar40xx_read(priv, AR40XX_REG_QM_DEBUG_VALUE);
		/* every 8 bits for each port */
		*qm_buffer_err = (qm_val >> ((port_id-4) * 8)) & 0xFF;
	}

	return 0;
}

static void
ar40xx_sw_mac_polling_task(struct ar40xx_priv *priv)
{
	static int task_count;
	u32 i;
	u32 reg, value;
	u32 link, speed, duplex;
	u32 qm_buffer_err;
	u16 port_phy_status[AR40XX_NUM_PORTS];
	static u32 qm_err_cnt[AR40XX_NUM_PORTS] = {0, 0, 0, 0, 0, 0};
	static u32 link_cnt[AR40XX_NUM_PORTS] = {0, 0, 0, 0, 0, 0};
	struct mii_bus *bus = NULL;

	if (!priv || !priv->mii_bus)
		return;

	bus = priv->mii_bus;

	++task_count;

	for (i = 1; i < AR40XX_NUM_PORTS; ++i) {
		port_phy_status[i] =
			mdiobus_read(bus, i-1, AR40XX_PHY_SPEC_STATUS);

		speed = FIELD_GET(AR40XX_PHY_SPEC_STATUS_SPEED,
				  port_phy_status[i]);
		link = FIELD_GET(AR40XX_PHY_SPEC_STATUS_LINK,
				 port_phy_status[i]);
		duplex = FIELD_GET(AR40XX_PHY_SPEC_STATUS_DUPLEX,
				   port_phy_status[i]);

		if (link != priv->ar40xx_port_old_link[i]) {
			++link_cnt[i];
			/* Up --> Down */
			if ((priv->ar40xx_port_old_link[i] ==
					AR40XX_PORT_LINK_UP) &&
			    (link == AR40XX_PORT_LINK_DOWN)) {
				/* LINK_EN disable(MAC force mode)*/
				reg = AR40XX_REG_PORT_STATUS(i);
				ar40xx_rmw(priv, reg,
						AR40XX_PORT_AUTO_LINK_EN, 0);

				/* Check queue buffer */
				qm_err_cnt[i] = 0;
				ar40xx_get_qm_status(priv, i, &qm_buffer_err);
				if (qm_buffer_err) {
					priv->ar40xx_port_qm_buf[i] =
						AR40XX_QM_NOT_EMPTY;
				} else {
					u16 phy_val = 0;

					priv->ar40xx_port_qm_buf[i] =
						AR40XX_QM_EMPTY;
					ar40xx_force_1g_full(priv, i);
					/* Ref:QCA8337 Datasheet,Clearing
					 * MENU_CTRL_EN prevents phy to
					 * stuck in 100BT mode when
					 * bringing up the link
					 */
					ar40xx_phy_dbg_read(priv, i-1,
							    AR40XX_PHY_DEBUG_0,
							    &phy_val);
					phy_val &= (~AR40XX_PHY_MANU_CTRL_EN);
					ar40xx_phy_dbg_write(priv, i-1,
							     AR40XX_PHY_DEBUG_0,
							     phy_val);
				}
				priv->ar40xx_port_old_link[i] = link;
			} else if ((priv->ar40xx_port_old_link[i] ==
						AR40XX_PORT_LINK_DOWN) &&
					(link == AR40XX_PORT_LINK_UP)) {
				/* Down --> Up */
				if (priv->port_link_up[i] < 1) {
					++priv->port_link_up[i];
				} else {
					/* Change port status */
					reg = AR40XX_REG_PORT_STATUS(i);
					value = ar40xx_read(priv, reg);
					priv->port_link_up[i] = 0;

					value &= ~(AR40XX_PORT_DUPLEX |
						   AR40XX_PORT_SPEED);
					value |= speed | (duplex ? BIT(6) : 0);
					ar40xx_write(priv, reg, value);
					/* clock switch need such time
					 * to avoid glitch
					 */
					usleep_range(100, 200);

					value |= AR40XX_PORT_AUTO_LINK_EN;
					ar40xx_write(priv, reg, value);
					/* HW need such time to make sure link
					 * stable before enable MAC
					 */
					usleep_range(100, 200);

					if (speed == AR40XX_PORT_SPEED_100M) {
						u16 phy_val = 0;
						/* Enable @100M, if down to 10M
						 * clock will change smoothly
						 */
						ar40xx_phy_dbg_read(priv, i-1,
								    0,
								    &phy_val);
						phy_val |=
							AR40XX_PHY_MANU_CTRL_EN;
						ar40xx_phy_dbg_write(priv, i-1,
								     0,
								     phy_val);
					}
					priv->ar40xx_port_old_link[i] = link;
				}
			}
		}

		if (priv->ar40xx_port_qm_buf[i] == AR40XX_QM_NOT_EMPTY) {
			/* Check QM */
			ar40xx_get_qm_status(priv, i, &qm_buffer_err);
			if (qm_buffer_err) {
				++qm_err_cnt[i];
			} else {
				priv->ar40xx_port_qm_buf[i] =
						AR40XX_QM_EMPTY;
				qm_err_cnt[i] = 0;
				ar40xx_force_1g_full(priv, i);
			}
		}
	}
}

static void
ar40xx_qm_err_check_work_task(struct work_struct *work)
{
	struct ar40xx_priv *priv = container_of(work, struct ar40xx_priv,
					qm_dwork.work);

	mutex_lock(&priv->qm_lock);

	ar40xx_sw_mac_polling_task(priv);

	mutex_unlock(&priv->qm_lock);

	schedule_delayed_work(&priv->qm_dwork,
			      msecs_to_jiffies(AR40XX_QM_WORK_DELAY));
}

static int
ar40xx_qm_err_check_work_start(struct ar40xx_priv *priv)
{
	mutex_init(&priv->qm_lock);

	INIT_DELAYED_WORK(&priv->qm_dwork, ar40xx_qm_err_check_work_task);

	schedule_delayed_work(&priv->qm_dwork,
			      msecs_to_jiffies(AR40XX_QM_WORK_DELAY));

	return 0;
}

/* End of qm error WAR */

static int
ar40xx_vlan_init(struct ar40xx_priv *priv)
{
	int port;
	unsigned long bmp;

	/* By default Enable VLAN */
	priv->vlan = 1;
	priv->vlan_table[AR40XX_LAN_VLAN] = priv->cpu_bmp | priv->lan_bmp;
	priv->vlan_table[AR40XX_WAN_VLAN] = priv->cpu_bmp | priv->wan_bmp;
	priv->vlan_tagged = priv->cpu_bmp;
	bmp = priv->lan_bmp;
	for_each_set_bit(port, &bmp, AR40XX_NUM_PORTS)
			priv->pvid[port] = AR40XX_LAN_VLAN;

	bmp = priv->wan_bmp;
	for_each_set_bit(port, &bmp, AR40XX_NUM_PORTS)
			priv->pvid[port] = AR40XX_WAN_VLAN;

	return 0;
}

static void
ar40xx_mib_work_func(struct work_struct *work)
{
	struct ar40xx_priv *priv;
	int err;

	priv = container_of(work, struct ar40xx_priv, mib_work.work);

	mutex_lock(&priv->mib_lock);

	err = ar40xx_mib_capture(priv);
	if (err)
		goto next_port;

	ar40xx_mib_fetch_port_stat(priv, priv->mib_next_port, false);

next_port:
	priv->mib_next_port++;
	if (priv->mib_next_port >= priv->dev.ports)
		priv->mib_next_port = 0;

	mutex_unlock(&priv->mib_lock);

	schedule_delayed_work(&priv->mib_work,
			      msecs_to_jiffies(AR40XX_MIB_WORK_DELAY));
}

static void
ar40xx_setup_port(struct ar40xx_priv *priv, int port, u32 members)
{
	u32 t;
	u32 egress, ingress;
	u32 pvid = priv->vlan_id[priv->pvid[port]];

	if (priv->vlan) {
		egress = AR40XX_PORT_VLAN1_OUT_MODE_UNMOD;

		ingress = AR40XX_IN_SECURE;
	} else {
		egress = AR40XX_PORT_VLAN1_OUT_MODE_UNTOUCH;
		ingress = AR40XX_IN_PORT_ONLY;
	}

	t = pvid << AR40XX_PORT_VLAN0_DEF_SVID_S;
	t |= pvid << AR40XX_PORT_VLAN0_DEF_CVID_S;
	ar40xx_write(priv, AR40XX_REG_PORT_VLAN0(port), t);

	t = AR40XX_PORT_VLAN1_PORT_VLAN_PROP;
	t |= egress << AR40XX_PORT_VLAN1_OUT_MODE_S;

	ar40xx_write(priv, AR40XX_REG_PORT_VLAN1(port), t);

	t = members;
	t |= AR40XX_PORT_LOOKUP_LEARN;
	t |= ingress << AR40XX_PORT_LOOKUP_IN_MODE_S;
	t |= AR40XX_PORT_STATE_FORWARD << AR40XX_PORT_LOOKUP_STATE_S;
	ar40xx_write(priv, AR40XX_REG_PORT_LOOKUP(port), t);
}

static void
ar40xx_vtu_op(struct ar40xx_priv *priv, u32 op, u32 val)
{
	if (ar40xx_wait_bit(priv, AR40XX_REG_VTU_FUNC1,
			    AR40XX_VTU_FUNC1_BUSY, 0))
		return;

	if ((op & AR40XX_VTU_FUNC1_OP) == AR40XX_VTU_FUNC1_OP_LOAD)
		ar40xx_write(priv, AR40XX_REG_VTU_FUNC0, val);

	op |= AR40XX_VTU_FUNC1_BUSY;
	ar40xx_write(priv, AR40XX_REG_VTU_FUNC1, op);
}

static void
ar40xx_vtu_load_vlan(struct ar40xx_priv *priv, u32 vid, u32 port_mask)
{
	u32 op;
	u32 val;
	int i;

	op = AR40XX_VTU_FUNC1_OP_LOAD | (vid << AR40XX_VTU_FUNC1_VID_S);
	val = AR40XX_VTU_FUNC0_VALID | AR40XX_VTU_FUNC0_IVL;
	for (i = 0; i < AR40XX_NUM_PORTS; i++) {
		u32 mode;

		if ((port_mask & BIT(i)) == 0)
			mode = AR40XX_VTU_FUNC0_EG_MODE_NOT;
		else if (priv->vlan == 0)
			mode = AR40XX_VTU_FUNC0_EG_MODE_KEEP;
		else if ((priv->vlan_tagged & BIT(i)) ||
			 (priv->vlan_id[priv->pvid[i]] != vid))
			mode = AR40XX_VTU_FUNC0_EG_MODE_TAG;
		else
			mode = AR40XX_VTU_FUNC0_EG_MODE_UNTAG;

		val |= mode << AR40XX_VTU_FUNC0_EG_MODE_S(i);
	}
	ar40xx_vtu_op(priv, op, val);
}

static void
ar40xx_vtu_flush(struct ar40xx_priv *priv)
{
	ar40xx_vtu_op(priv, AR40XX_VTU_FUNC1_OP_FLUSH, 0);
}

static int
ar40xx_sw_hw_apply(struct switch_dev *dev)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);
	u8 portmask[AR40XX_NUM_PORTS];
	int i, j;

	mutex_lock(&priv->reg_mutex);
	/* flush all vlan entries */
	ar40xx_vtu_flush(priv);

	memset(portmask, 0, sizeof(portmask));
	if (priv->vlan) {
		for (j = 0; j < AR40XX_MAX_VLANS; j++) {
			u8 vp = priv->vlan_table[j];

			if (!vp)
				continue;

			for (i = 0; i < dev->ports; i++) {
				u8 mask = BIT(i);

				if (vp & mask)
					portmask[i] |= vp & ~mask;
			}

			ar40xx_vtu_load_vlan(priv, priv->vlan_id[j],
					     priv->vlan_table[j]);
		}
	} else {
		/* 8021q vlan disabled */
		for (i = 0; i < dev->ports; i++) {
			if (i == AR40XX_PORT_CPU)
				continue;

			portmask[i] = BIT(AR40XX_PORT_CPU);
			portmask[AR40XX_PORT_CPU] |= BIT(i);
		}
	}

	/* update the port destination mask registers and tag settings */
	for (i = 0; i < dev->ports; i++)
		ar40xx_setup_port(priv, i, portmask[i]);

	ar40xx_set_mirror_regs(priv);

	mutex_unlock(&priv->reg_mutex);
	return 0;
}

static int
ar40xx_sw_reset_switch(struct switch_dev *dev)
{
	struct ar40xx_priv *priv = swdev_to_ar40xx(dev);
	int i, rv;

	mutex_lock(&priv->reg_mutex);
	memset(&priv->vlan, 0, sizeof(struct ar40xx_priv) -
		offsetof(struct ar40xx_priv, vlan));

	for (i = 0; i < AR40XX_MAX_VLANS; i++)
		priv->vlan_id[i] = i;

	ar40xx_vlan_init(priv);

	priv->mirror_rx = false;
	priv->mirror_tx = false;
	priv->source_port = 0;
	priv->monitor_port = 0;

	mutex_unlock(&priv->reg_mutex);

	rv = ar40xx_sw_hw_apply(dev);
	return rv;
}

static int
ar40xx_start(struct ar40xx_priv *priv)
{
	int ret;

	ret = ar40xx_hw_init(priv);
	if (ret)
		return ret;

	ret = ar40xx_sw_reset_switch(&priv->dev);
	if (ret)
		return ret;

	/* at last, setup cpu port */
	ret = ar40xx_cpuport_setup(priv);
	if (ret)
		return ret;

	schedule_delayed_work(&priv->mib_work,
			      msecs_to_jiffies(AR40XX_MIB_WORK_DELAY));

	ar40xx_qm_err_check_work_start(priv);

	return 0;
}

static const struct switch_dev_ops ar40xx_sw_ops = {
	.attr_global = {
		.attr = ar40xx_sw_attr_globals,
		.n_attr = ARRAY_SIZE(ar40xx_sw_attr_globals),
	},
	.attr_port = {
		.attr = ar40xx_sw_attr_port,
		.n_attr = ARRAY_SIZE(ar40xx_sw_attr_port),
	},
	.attr_vlan = {
		.attr = ar40xx_sw_attr_vlan,
		.n_attr = ARRAY_SIZE(ar40xx_sw_attr_vlan),
	},
	.get_port_pvid = ar40xx_sw_get_pvid,
	.set_port_pvid = ar40xx_sw_set_pvid,
	.get_vlan_ports = ar40xx_sw_get_ports,
	.set_vlan_ports = ar40xx_sw_set_ports,
	.apply_config = ar40xx_sw_hw_apply,
	.reset_switch = ar40xx_sw_reset_switch,
	.get_port_link = ar40xx_sw_get_port_link,
	.get_port_stats = ar40xx_sw_get_port_stats,
};

/* Platform driver probe function */

static int ar40xx_probe(struct platform_device *pdev)
{
	struct device_node *switch_node;
	struct device_node *psgmii_node;
	struct device_node *mdio_node;
	const __be32 *mac_mode;
	struct clk *ess_clk;
	struct switch_dev *swdev;
	struct ar40xx_priv *priv;
	u32 len;
	u32 num_mibs;
	struct resource psgmii_base = {0};
	struct resource switch_base = {0};
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);
	ar40xx_priv = priv;

	switch_node = of_node_get(pdev->dev.of_node);
	if (of_address_to_resource(switch_node, 0, &switch_base) != 0)
		return -EIO;

	priv->hw_addr = devm_ioremap_resource(&pdev->dev, &switch_base);
	if (IS_ERR(priv->hw_addr)) {
		dev_err(&pdev->dev, "Failed to ioremap switch_base!\n");
		return PTR_ERR(priv->hw_addr);
	}

	/*psgmii dts get*/
	psgmii_node = of_find_node_by_name(NULL, "ess-psgmii");
	if (!psgmii_node) {
		dev_err(&pdev->dev, "Failed to find ess-psgmii node!\n");
		return -EINVAL;
	}

	if (of_address_to_resource(psgmii_node, 0, &psgmii_base) != 0)
		return -EIO;

	priv->psgmii_hw_addr = devm_ioremap_resource(&pdev->dev, &psgmii_base);
	if (IS_ERR(priv->psgmii_hw_addr)) {
		dev_err(&pdev->dev, "psgmii ioremap fail!\n");
		return PTR_ERR(priv->psgmii_hw_addr);
	}

	mac_mode = of_get_property(switch_node, "switch_mac_mode", &len);
	if (!mac_mode) {
		dev_err(&pdev->dev, "Failed to read switch_mac_mode\n");
		return -EINVAL;
	}
	priv->mac_mode = be32_to_cpup(mac_mode);

	ess_clk = of_clk_get_by_name(switch_node, "ess_clk");
	if (ess_clk)
		clk_prepare_enable(ess_clk);

	priv->ess_rst = devm_reset_control_get(&pdev->dev, "ess_rst");
	if (IS_ERR(priv->ess_rst)) {
		dev_err(&pdev->dev, "Failed to get ess_rst control!\n");
		return PTR_ERR(priv->ess_rst);
	}

	if (of_property_read_u32(switch_node, "switch_cpu_bmp",
				 &priv->cpu_bmp) ||
	    of_property_read_u32(switch_node, "switch_lan_bmp",
				 &priv->lan_bmp) ||
	    of_property_read_u32(switch_node, "switch_wan_bmp",
				 &priv->wan_bmp)) {
		dev_err(&pdev->dev, "Failed to read port properties\n");
		return -EIO;
	}

	mutex_init(&priv->reg_mutex);
	mutex_init(&priv->mib_lock);
	INIT_DELAYED_WORK(&priv->mib_work, ar40xx_mib_work_func);

	/* register switch */
	swdev = &priv->dev;

	mdio_node = of_find_compatible_node(NULL, NULL, "qcom,ipq4019-mdio");
	if (!mdio_node) {
		dev_err(&pdev->dev, "Probe failed - Cannot find mdio node by phandle!\n");
		ret = -ENODEV;
		goto err_missing_phy;
	}

	priv->mii_bus = of_mdio_find_bus(mdio_node);

	if (priv->mii_bus == NULL) {
		dev_err(&pdev->dev, "Probe failed - Missing PHYs!\n");
		ret = -ENODEV;
		goto err_missing_phy;
	}

	swdev->alias = dev_name(&priv->mii_bus->dev);

	swdev->cpu_port = AR40XX_PORT_CPU;
	swdev->name = "QCA AR40xx";
	swdev->vlans = AR40XX_MAX_VLANS;
	swdev->ports = AR40XX_NUM_PORTS;
	swdev->ops = &ar40xx_sw_ops;
	ret = register_switch(swdev, NULL);
	if (ret < 0) {
		dev_err(&pdev->dev, "Switch registration failed!\n");
		return ret;
	}

	num_mibs = ARRAY_SIZE(ar40xx_mibs);
	len = priv->dev.ports * num_mibs *
	      sizeof(*priv->mib_stats);
	priv->mib_stats = devm_kzalloc(&pdev->dev, len, GFP_KERNEL);
	if (!priv->mib_stats) {
		ret = -ENOMEM;
		goto err_unregister_switch;
	}

	ar40xx_start(priv);

	return 0;

err_unregister_switch:
	unregister_switch(&priv->dev);
err_missing_phy:
	platform_set_drvdata(pdev, NULL);
	return ret;
}

static int ar40xx_remove(struct platform_device *pdev)
{
	struct ar40xx_priv *priv = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&priv->qm_dwork);
	cancel_delayed_work_sync(&priv->mib_work);

	unregister_switch(&priv->dev);

	return 0;
}

static const struct of_device_id ar40xx_of_mtable[] = {
	{.compatible = "qcom,ess-switch" },
	{}
};

struct platform_driver ar40xx_drv = {
	.probe = ar40xx_probe,
	.remove = ar40xx_remove,
	.driver = {
		.name    = "ar40xx",
		.of_match_table = ar40xx_of_mtable,
	},
};

module_platform_driver(ar40xx_drv);

MODULE_DESCRIPTION("IPQ40XX ESS driver");
MODULE_LICENSE("Dual BSD/GPL");
